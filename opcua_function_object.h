#pragma once

#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <functional>

#include <QObject>
#include <QString>
#include <QVariant>
#include <QVector>
#include <QMetaEnum>
#include <QMetaType>

#include <QUaBaseObject>
#include <QUaProperty>
#include <QUaServer>

#include <template_defines.h>

/**
 * @file opcua_function_object.h
 * @brief Expose a C++ callable as an OPC UA Object with one Property per
 *        argument — a portable workaround for clients that do not support
 *        OPC UA Methods.
 *
 * Required toolkit: https://github.com/96Ven96/cpp-template-toolkit
 *  (uses FunctionTraitsNs::CallableFn for compile-time callable
 *   introspection and TypesHelperNs::isValidIndex for index checks).
 */

/**
 * @brief Bundle of OPC UA node names — replaces the legacy 4-tuple of
 *        @c const char* parameters.
 *
 * Each field is a @c std::string_view, so the struct is constexpr-friendly
 * and lightweight to pass around. Empty fields are treated as "not
 * provided":
 *  - @c uriName empty   → falls back to @c browseName for the node id;
 *  - @c displayName / @c description empty → the corresponding setter is
 *    not invoked.
 */
struct OpcuaNodeNames {
    std::string_view browseName;
    std::string_view uriName{};
    std::string_view displayName{};
    std::string_view description{};
};


class OpcuaFunctionObject;


namespace OpcuaFunctionObjectNs {

    namespace Detail {

        /// @brief Hierarchy separator used when composing node ids.
        constexpr char UriSeparator = '/';

        /// @brief Variadic dispatcher used to apply a functor to each index in a sequence.
        template <typename Fn, std::size_t... Is>
        constexpr void runIndexLoop(Fn& fn, std::index_sequence<Is...>) {
            (fn(std::integral_constant<std::size_t, Is>{}), ...);
        }

        /// @brief Convert a string_view to QString, returning an empty QString for empty views.
        [[nodiscard]] inline QString toQString(std::string_view sv) noexcept {
            return sv.empty()
                ? QString{}
                : QString::fromUtf8(sv.data(), static_cast<int>(sv.size()));
        }

        /// @brief Compose a child node id of the form "<parent><separator><child>".
        [[nodiscard]] inline QString composedIdName(const QString& parentName,
                                                     std::string_view childName) noexcept
        {
            const QString parent = parentName.isEmpty()
                ? QStringLiteral("-")
                : parentName;
            return parent + QChar(UriSeparator) + toQString(childName);
        }

        /// @brief Build a QUaNodeId for a child of @p parent identified by @p childKey.
        [[nodiscard]] inline QUaNodeId childNodeId(const QUaNodeId& parent,
                                                    std::string_view childKey) noexcept
        {
            const auto parentIdStr = parent.stringId();
            Q_ASSERT_X(!parentIdStr.isEmpty(), Q_FUNC_INFO,
                       "Hierarchical id is not defined");
            return QUaNodeId{parent.namespaceIndex(),
                             composedIdName(parentIdStr, childKey)};
        }

        /// @brief Produce a (QualifiedName, NodeId) pair from a parent node and a names bundle.
        [[nodiscard]] inline std::pair<QUaQualifiedName, QUaNodeId>
        makeQualifiedAndNodeId(const QUaNodeId& parent,
                                const OpcuaNodeNames& names) noexcept
        {
            QUaQualifiedName qualiName;
            qualiName.setNamespaceIndex(parent.namespaceIndex());
            qualiName.setName(toQString(names.browseName));

            const auto idSource = names.uriName.empty()
                ? names.browseName
                : names.uriName;
            return std::pair{qualiName, childNodeId(parent, idSource)};
        }

        /// @brief Apply display name and description on @p node, skipping empty fields.
        template <typename NodeT>
        inline NodeT* applyDisplayNameAndDescription(NodeT* node,
                                                      const OpcuaNodeNames& names) noexcept
        {
            if (!names.displayName.empty()) {
                node->setDisplayName(toQString(names.displayName));
            }
            if (!names.description.empty()) {
                node->setDescription(toQString(names.description));
            }
            return node;
        }

        /// @brief Add a QUaProperty child to @p parent using @p names.
        template <typename ParentT>
        [[nodiscard]] inline QUaProperty* addPropertyTo(ParentT* parent,
                                                         const OpcuaNodeNames& names) noexcept
        {
            auto [qualiName, newNode] = makeQualifiedAndNodeId(parent->nodeId(), names);
            QUaProperty* property = parent->addProperty(qualiName, newNode);
            return applyDisplayNameAndDescription(property, names);
        }

        /// @brief Add a typed @c ChildT child to @p parent using @p names.
        template <typename ChildT, typename ParentT>
        [[nodiscard]] inline ChildT* addTypedChildTo(ParentT* parent,
                                                      const OpcuaNodeNames& names) noexcept
        {
            auto [qualiName, newNode] = makeQualifiedAndNodeId(parent->nodeId(), names);
            ChildT* child = parent->template addChild<ChildT>(qualiName, newNode);
            return applyDisplayNameAndDescription(child, names);
        }

    } // namespace Detail

} // namespace OpcuaFunctionObjectNs


/**
 * @brief Expose a C++ callable as an OPC UA Object with one writable
 *        Property per argument.
 *
 * The motivation: OPC UA defines a @c Method node kind for callable
 * operations, but many real-world OPC UA clients (lightweight SCADA
 * front-ends, dashboard / IoT cloud connectors, web bridges) do not
 * support it — they can only read and write Variables. Domain logic that
 * needs to be invokable from those clients gets stuck.
 *
 * @c OpcuaFunctionObject is the portable workaround. It exposes any
 * @c bool() callable as a Qt OPC UA Object whose children are:
 *
 *  - one writable @b QUaProperty per callable argument, configured with the
 *    matching Qt meta-type;
 *  - a @b Status property reporting one of
 *    @c None / @c Processing / @c Completed / @c Failed (exposed as an
 *    OPC UA enum via @c Q_ENUM);
 *  - an @b Apply boolean property — set it to @c true and the function fires.
 *
 * The function's signature is reflected at compile time via the
 * @c FunctionTraitsNs::CallableFn machinery from the companion template
 * toolkit. Argument types are deduced from the callable itself and used
 * to configure the underlying property meta-types, so the user does not
 * have to enumerate them by hand.
 *
 * @par Usage
 * @code
 *   bool myFunction(int durationMs, QString message);
 *
 *   auto* fn = OpcuaFunctionObjectFactory::addAsFlatFunction(
 *       &myFunction,
 *       OpcuaNodeNames{ "DoSomething",  "DoSomething",
 *                       "Do Something", "Trigger the action" },
 *       QVector<OpcuaNodeNames>{
 *           { "Duration", "Duration", "Duration [ms]",  "" },
 *           { "Message",  "Message",  "Message",        "" },
 *       },
 *       parentObject);
 * @endcode
 *
 * The callable @b must return @c bool. The return value populates the
 * @c Status property: @c true → @c Completed, @c false → @c Failed.
 */
class OpcuaFunctionObject : public QUaBaseObject
{
    Q_OBJECT

public:

    enum State {
        Completed,
        None,
        Failed,
        Processing,
    };
    Q_ENUM(State)

    /// @brief Default node names for the auto-created @b Status property.
    static constexpr OpcuaNodeNames StatusNames {
        "Status", "Status", "Execution status", "Function execution status"
    };

    /// @brief Default node names for the auto-created @b Apply property.
    static constexpr OpcuaNodeNames ApplyNames {
        "Apply", "Apply", "Apply", "Set to true to trigger the function"
    };

    /// @brief Required by QUaServer for type registration / dynamic creation.
    Q_INVOKABLE explicit OpcuaFunctionObject(QUaServer* server);

    /**
     * @brief Bind a callable. Creates one writable QUaProperty per parameter.
     *
     * @param callable        Any callable returning @c bool.
     * @param parameterNames  One entry per callable argument, in order.
     *                        Must match the callable's arity exactly.
     */
    template <typename Callable>
    void setCallable(Callable&& callable,
                     const QVector<OpcuaNodeNames>& parameterNames) noexcept;

private:

    using InternalCallableT = std::function<bool(void)>;

    template <typename T>
    [[nodiscard]] static std::optional<T> readFromProperty(const QUaProperty* prop) noexcept;

    template <typename T>
    static void configurePropertyType(QUaProperty* prop) noexcept;

    template <typename Callable, typename Tuple, std::size_t... Is>
    [[nodiscard]] bool invokeFromPropertiesImpl(Callable& fn,
                                                 std::index_sequence<Is...>) noexcept;

    template <typename Callable, typename Tuple, std::size_t ArgsCount>
    static InternalCallableT produceInvoker(OpcuaFunctionObject* self,
                                              Callable&& fn) noexcept
    {
        return [self, fn = std::forward<Callable>(fn)]() mutable -> bool {
            return self->invokeFromPropertiesImpl<Callable, Tuple>(
                fn, std::make_index_sequence<ArgsCount>{});
        };
    }

    void onApplyFunction() noexcept;

    QUaProperty* m_state = nullptr;
    QUaProperty* m_apply = nullptr;

    InternalCallableT      m_functionLogic = [] { return true; };
    QVector<QUaProperty*>  m_inputs;
};


/**
 * @brief Free-standing factory helpers for @c OpcuaFunctionObject.
 */
namespace OpcuaFunctionObjectFactory {

    /**
     * @brief Create an OpcuaFunctionObject as a child of @p parent and bind
     *        @p callable to it in a single call.
     */
    template <typename Callable, typename ParentT>
    inline OpcuaFunctionObject* addAsFlatFunction(
            Callable&& callable,
            const OpcuaNodeNames& functionNames,
            const QVector<OpcuaNodeNames>& parameterNames,
            ParentT* parent) noexcept
    {
        Q_ASSERT(parent != nullptr);
        OpcuaFunctionObject* fn =
            OpcuaFunctionObjectNs::Detail::addTypedChildTo<OpcuaFunctionObject>(
                parent, functionNames);
        fn->setCallable(std::forward<Callable>(callable), parameterNames);
        return fn;
    }

} // namespace OpcuaFunctionObjectFactory


// =============================================================================
// Inline / template definitions
// =============================================================================

template <typename Callable, typename Tuple, std::size_t... Is>
inline bool OpcuaFunctionObject::invokeFromPropertiesImpl(
        Callable& callable, std::index_sequence<Is...>) noexcept
{
    namespace CFn = FunctionTraitsNs::CallableFn;
    using ReturnT = typename CFn::template return_t<std::decay_t<Callable>>;
    static_assert(std::is_same_v<ReturnT, bool>,
                  "OpcuaFunctionObject: Callable must return bool.");

    // Materialise each argument by reading it back from its bound property.
    bool ok = true;
    auto makeArg = [this, &ok](auto idxC) {
        constexpr std::size_t Idx = decltype(idxC)::value;
        using ArgI  = std::tuple_element_t<Idx, Tuple>;
        using Clean = std::remove_cv_t<std::remove_reference_t<ArgI>>;

        Q_ASSERT(TypesHelperNs::isValidIndex(m_inputs, Idx));
        auto opt = readFromProperty<Clean>(m_inputs[static_cast<int>(Idx)]);
        if (!opt.has_value()) {
            ok = false;
            return Clean{};
        }
        return std::move(*opt); // extract from the optional
    };

    Tuple args{ makeArg(std::integral_constant<std::size_t, Is>{})... };

    if (!ok) {
        return false;
    }
    return std::apply(callable, args);
}

template <typename T>
inline void OpcuaFunctionObject::configurePropertyType(QUaProperty* prop) noexcept
{
    using ValT = std::remove_cv_t<std::remove_reference_t<T>>;
    const int id = qMetaTypeId<ValT>();

    Q_ASSERT(prop != nullptr);
    Q_ASSERT_X(id != QMetaType::UnknownType, Q_FUNC_INFO,
               "Type not registered with Qt (Q_DECLARE_METATYPE / qRegisterMetaType)");

    // Most QUa builds accept QMetaType::Type for built-in types.
    prop->setDataType(static_cast<QMetaType::Type>(id));
    prop->setWriteAccess(true);
}

template <typename T>
inline std::optional<T> OpcuaFunctionObject::readFromProperty(const QUaProperty* prop) noexcept
{
    using ValT = std::remove_cv_t<std::remove_reference_t<T>>;

    QVariant val = prop->value();
    const int targetId = qMetaTypeId<ValT>();
    if (!val.canConvert(targetId)) {
        return std::nullopt;
    }
    if (!val.convert(targetId)) {
        return std::nullopt;
    }
    return val.value<ValT>();
}

template <typename Callable>
inline void OpcuaFunctionObject::setCallable(
        Callable&& callable,
        const QVector<OpcuaNodeNames>& parameterNames) noexcept
{
    namespace CFn = FunctionTraitsNs::CallableFn;
    using ReturnT = typename CFn::template return_t<std::decay_t<Callable>>;
    using Decayed = std::decay_t<Callable>;
    using Tuple   = typename CFn::template input_t<Decayed>;
    constexpr std::size_t ArgsCount = std::tuple_size_v<Tuple>;

    static_assert(std::is_same_v<ReturnT, bool>,
                  "OpcuaFunctionObject: Callable must return bool.");

    // 1) Mismatch between argument count and description list.
    if (parameterNames.size() != static_cast<int>(ArgsCount)) {
        Q_ASSERT_X(false, Q_FUNC_INFO,
                   "parameterNames size does not match the callable arity");
        m_functionLogic = [] { return false; };
        return;
    }

    // 2) Reset any pre-existing input properties.
    qDeleteAll(m_inputs);
    m_inputs.clear();
    m_inputs.reserve(static_cast<int>(ArgsCount));

    // 3) Create one writable property per argument, with the matching meta-type.
    auto addOne = [this, &parameterNames](auto idxC) {
        constexpr std::size_t I = decltype(idxC)::value;
        using ArgI  = std::tuple_element_t<I, Tuple>;
        using Clean = std::remove_cv_t<std::remove_reference_t<ArgI>>;

        QUaProperty* property = OpcuaFunctionObjectNs::Detail::addPropertyTo(
            this, parameterNames[static_cast<int>(I)]);
        configurePropertyType<Clean>(property);
        m_inputs.push_back(property);
    };
    OpcuaFunctionObjectNs::Detail::runIndexLoop(addOne,
                                                 std::make_index_sequence<ArgsCount>{});

    // 4) Capture the callable by value and produce the invocation closure.
    Decayed fn = std::forward<Callable>(callable);
    m_functionLogic = produceInvoker<Decayed, Tuple, ArgsCount>(this, std::move(fn));
}

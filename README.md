# OPC UA Function Object

A drop-in C++ class that exposes any `bool` callable as an **OPC UA Object
with input properties** — a portable workaround for OPC UA clients that
do not support OPC UA Methods.

Built on top of [QUaServer](https://github.com/juangburgos/QUaServer)
(a Qt wrapper around [open62541](https://github.com/open62541/open62541)).

```cpp
bool setIntegrationTime(int ms);

auto* fn = OpcuaFunctionObjectFactory::addAsFlatFunction(
    &setIntegrationTime,
    OpcuaNodeNames{ "SetIntegrationTime", "SetIntegrationTime",
                    "Set integration time", "Configure the spectrometer" },
    QVector<OpcuaNodeNames>{
        { "Ms", "Ms", "Integration time [ms]", "" },
    },
    deviceNode);
```

From any OPC UA client (even one that only does read/write), the user now sees:

```
Device
└── SetIntegrationTime          (Object)
    ├── Ms        : Int32       (writable Property)
    ├── Status    : enum        (None / Processing / Completed / Failed)
    └── Apply     : Bool        (writable Property — set true to invoke)
```

Write the input(s), flip `Apply` to `true`, observe `Status`. Works in
every OPC UA client that handles Variables — i.e. essentially all of them.

---

## Why this exists

OPC UA defines a [Method](https://reference.opcfoundation.org/v104/Core/docs/Part4/5.11/)
node kind for callable operations. In practice:

- many lightweight clients (dashboards, IoT cloud connectors, browser-based
  HMIs, web bridges, several SCADA front-ends) **do not implement Method
  calls** — they only handle Variables;
- supporting them requires writing the Method discovery + invocation flow,
  which is not trivial;
- as a result, domain logic that must be triggered from those clients ends
  up either re-implemented as a side-channel REST API, or simply unreachable.

This class is a **portable workaround**: it represents a function as an
*Object* whose Properties are the function's parameters. Writing the
properties is just a normal Variable write; triggering is just flipping a
boolean. Every OPC UA client supports that.

The cost is a small protocol round-trip (write inputs, write Apply, poll
Status). For the typical use case — calibration, configuration, manual
operations — that overhead is irrelevant compared to the operational gain.

---

## How it works

`OpcuaFunctionObject` is a `QUaBaseObject` subclass. `setCallable(fn, names)`
inspects the callable's signature **at compile time** via the companion
[cpp-template-toolkit](https://github.com/96Ven96/cpp-template-toolkit),
extracts the argument tuple, and for each argument:

1. creates a writable `QUaProperty` whose `dataType` matches the argument's
   Qt meta-type (`qMetaTypeId<T>()`);
2. stores the property pointer in `m_inputs[i]`.

When the client writes `Apply = true`, the slot `onApplyFunction()`:

1. flips `Status` to `Processing`;
2. reads each property back into the right type via `QVariant::convert`
   and `QVariant::value<T>()` — this is where late-binding happens;
3. invokes the captured callable via `std::apply` on the materialised tuple;
4. flips `Status` to `Completed` or `Failed` based on the boolean return;
5. resets `Apply` to `false`.

The whole thing is a small, focused state machine — no virtual dispatch,
no allocations on the hot path beyond what `QVariant` already does.

---

## File overview

| File | Role |
|---|---|
| `opcua_function_object.h` | `OpcuaNodeNames` struct, internal Detail helpers, `OpcuaFunctionObject` class, `OpcuaFunctionObjectFactory::addAsFlatFunction` |
| `opcua_function_object.cpp` | Constructor and `onApplyFunction` implementation |

Pull the two files into your project, link against QUaServer and the
companion template-toolkit, and you're set.

---

## `OpcuaNodeNames` — node names without positional confusion

This library uses a small struct instead of a tuple of `const char*`:

```cpp
struct OpcuaNodeNames {
    std::string_view browseName;
    std::string_view uriName{};      // empty → falls back to browseName
    std::string_view displayName{};  // empty → skipped
    std::string_view description{};  // empty → skipped
};
```

`std::string_view` keeps the struct **constexpr-friendly** — you can declare
all your node names as `static constexpr` objects (just like the original
4-tuple approach) but with named fields, default values, and no positional
mismatches between `uriName` and `displayName`.

```cpp
static constexpr OpcuaNodeNames Names{
    "DoSomething",                // browseName
    {},                           // uriName  (defaults to browseName)
    "Do Something",               // displayName
    "Trigger the action"          // description
};
```

The class itself ships with two defaults used internally:

```cpp
OpcuaFunctionObject::StatusNames {
    "Status", "Status", "Execution status", "Function execution status"
}
OpcuaFunctionObject::ApplyNames {
    "Apply", "Apply", "Apply", "Set to true to trigger the function"
}
```

---

## End-to-end example

```cpp
#include "opcua_function_object.h"
#include <QUaServer>

bool setIntegrationTimeMs(qint32 ms);          // your real function
bool selectModelByName(QString name);          // another one

void setupServer(QUaServer& server) {
    // Register the type so QUaServer can build the OPC UA model.
    server.registerType<OpcuaFunctionObject>();

    auto* root = server.objectsFolder();

    OpcuaFunctionObjectFactory::addAsFlatFunction(
        &setIntegrationTimeMs,
        OpcuaNodeNames{ "SetIntegrationTimeMs", "SetIntegrationTimeMs",
                        "Set integration time", "" },
        QVector<OpcuaNodeNames>{
            { "Ms", "Ms", "Integration time [ms]", "" },
        },
        root);

    OpcuaFunctionObjectFactory::addAsFlatFunction(
        &selectModelByName,
        OpcuaNodeNames{ "SelectModel", "SelectModel",
                        "Select model", "Switch the analytical model" },
        QVector<OpcuaNodeNames>{
            { "Name", "Name", "Model name", "" },
        },
        root);
}
```

A client connected to that server sees the two functions as Objects, each
with its parameter property, a Status, and an Apply trigger.

---

## Requirements

- **Qt** 5.15+ or 6.x.
- **QUaServer** ≥ recent release (https://github.com/juangburgos/QUaServer).
- **C++20** (the implementation uses concepts, fold expressions, and
  `std::string_view` literals; the dependent template toolkit also requires
  C++20 for some paths).
- The companion [cpp-template-toolkit](https://github.com/96Ven96/cpp-template-toolkit)
  for `FunctionTraitsNs::CallableFn` and `TypesHelperNs::isValidIndex`.

---

## Limitations

- The callable **must return `bool`**. The boolean drives the `Status`
  transition (`true` → `Completed`, `false` → `Failed`). If you want
  richer return information, expose it as an extra output property on the
  same Object — not done here on purpose to keep the class minimal.
- All arguments must be **Qt-meta-type-registered**
  (`Q_DECLARE_METATYPE` / `qRegisterMetaType`). Anything that round-trips
  through `QVariant` works; custom types need the usual registration.
- Like all `QObject` code, everything runs on the thread that owns the
  object. Long-running callables block the OPC UA server thread unless
  you dispatch internally.

---

## Bug fixes and refactor

This code is a cleaned-up version of an internal project file:

- Replaced the legacy 4-tuple of `const char*` with the `OpcuaNodeNames`
  struct — same constexpr semantics, named fields, no positional bugs.
- Split the single `.h`-with-inline-`.cpp` file into a proper `.h` + `.cpp`.
- Fixed a copy-paste bug in `addDisplayNameAndDescription` (the description
  branch was forwarding the wrong template parameter).
- Fixed a `static_assert` in the stack-processing helper that was checking
  the callable's arity against each argument individually instead of the
  full argument list.
- Translated comments and identifiers from Italian to English.
- Migrated namespaces to the
  [cpp-template-toolkit](https://github.com/96Ven96/cpp-template-toolkit)
  layout (`FunctionTraitsNs::CallableFn`, `TypesHelperNs`).
- Added Doxygen on all public surface.
- Removed all references to the (proprietary) business-domain constants
  the original file pulled in; defaults for `Status` and `Apply` are now
  baked into the class itself.

---

## License

Released under the [MIT License](LICENSE) — free to use, modify, and
redistribute, including in commercial and closed-source projects.

---

## Contact

- GitHub — [@96Ven96](https://github.com/96Ven96)
- LinkedIn — [Leonardo Rossi](https://www.linkedin.com/in/leonardo-rossi-262641203)

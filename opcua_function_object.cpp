#include "opcua_function_object.h"

OpcuaFunctionObject::OpcuaFunctionObject(QUaServer* server)
    : QUaBaseObject{server}
{
    namespace D = OpcuaFunctionObjectNs::Detail;

    // Status property — exposes the per-invocation state as an OPC UA enum.
    m_state = D::addPropertyTo(this, StatusNames);
    m_state->setDataTypeEnum(QMetaEnum::fromType<OpcuaFunctionObject::State>());
    m_state->setValue(OpcuaFunctionObject::None);

    // Apply property — the trigger. The client writes "true" to invoke the
    // bound callable; the property is reset to "false" once execution ends.
    m_apply = D::addPropertyTo(this, ApplyNames);
    m_apply->setDataType(QMetaType::Bool);
    m_apply->setWriteAccess(true);

    connect(m_apply, &QUaProperty::valueChanged,
            this,    &OpcuaFunctionObject::onApplyFunction);
}

void OpcuaFunctionObject::onApplyFunction() noexcept
{
    if (!m_apply->value().toBool()) {
        // Apply was set back to false (likely by ourselves after a previous
        // run) — nothing to do here.
        return;
    }
    m_state->setValue(OpcuaFunctionObject::Processing);
    const bool res = m_functionLogic();
    m_state->setValue(res ? OpcuaFunctionObject::Completed
                          : OpcuaFunctionObject::Failed);
    m_apply->setValue(false);
}

#pragma once

#include "cooperator_var.h"
#include "cooperator.h"

namespace coop
{

template<typename T>
T* CooperatorVar<T>::Get()
{
    return reinterpret_cast<T*>(
        Cooperator::thread_cooperator->m_localStorage + m_offset);
}

template<typename T>
const T* CooperatorVar<T>::Get() const
{
    return reinterpret_cast<const T*>(
        Cooperator::thread_cooperator->m_localStorage + m_offset);
}

template<typename T>
T* CooperatorVar<T>::Get(Cooperator* coop)
{
    return reinterpret_cast<T*>(coop->m_localStorage + m_offset);
}

template<typename T>
const T* CooperatorVar<T>::Get(Cooperator* coop) const
{
    return reinterpret_cast<const T*>(coop->m_localStorage + m_offset);
}

} // end namespace coop

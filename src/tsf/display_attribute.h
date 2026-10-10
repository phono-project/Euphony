#pragma once
#include <msctf.h>

namespace euphony::tsf {
HRESULT enum_attributes(IEnumTfDisplayAttributeInfo** result);
HRESULT get_attribute(REFGUID guid, ITfDisplayAttributeInfo** result);
}

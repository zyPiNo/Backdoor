#pragma once

#include "HandleInfo.g.h"

namespace winrt::StarlightGUI::implementation
{
	struct HandleInfo : HandleInfoT<HandleInfo>
	{
		HandleInfo() = default;

		hstring Type() { return m_type; }
		void Type(hstring const& value) { m_type = value; }

		hstring Object() { return m_object; }
		void Object(hstring const& value) { m_object = value; }

		hstring Handle() { return m_handle; }
		void Handle(hstring const& value) { m_handle = value; }

		hstring Access() { return m_access; }
		void Access(hstring const& value) { m_access = value; }

		hstring Attributes() { return m_attributes; }
		void Attributes(hstring const& value) { m_attributes = value; }

	private:
		hstring m_type{ L"" };
		hstring m_object{ L"" };
		hstring m_handle{ L"" };
		hstring m_access{ L"" };
		hstring m_attributes{ L"" };
	};
}

namespace winrt::StarlightGUI::factory_implementation
{
	struct HandleInfo : HandleInfoT<HandleInfo, implementation::HandleInfo>
	{
	};
}
#pragma once

#include "ObjectEntry.g.h"

namespace winrt::StarlightGUI::implementation
{
	struct ObjectEntry : ObjectEntryT<ObjectEntry>
	{
		ObjectEntry() = default;

		hstring Name() { return m_name; }
		void Name(hstring const& value) { m_name = value; }

		hstring Path() { return m_path; }
		void Path(hstring const& value) { m_path = value; }

		hstring Type() { return m_type; }
		void Type(hstring const& value) { m_type = value; }

		bool Permanent() { return m_permanent; }
		void Permanent(bool value) { m_permanent = value; }

		ULONG References() { return m_references; }
		void References(ULONG value) { m_references = value; }

		ULONG Handles() { return m_handles; }
		void Handles(ULONG value) { m_handles = value; }

		ULONG PagedPool() { return m_pagedPool; }
		void PagedPool(ULONG value) { m_pagedPool = value; }

		ULONG NonPagedPool() { return m_nonPagedPool; }
		void NonPagedPool(ULONG value) { m_nonPagedPool = value; }

		hstring CreationTime() { return m_creationTime; }
		void CreationTime(hstring const& value) { m_creationTime = value; }

		hstring Link() { return m_link; }
		void Link(hstring const& value) { m_link = value; }

		hstring EventType() { return m_eventType; }
		void EventType(hstring const& value) { m_eventType = value; }

		bool EventSignaled() { return m_eventSignaled; }
		void EventSignaled(bool value) { m_eventSignaled = value; }

		ULONG MutantHoldCount() { return m_mutantHoldCount; }
		void MutantHoldCount(ULONG value) { m_mutantHoldCount = value; }

		bool MutantAbandoned() { return m_mutantAbandoned; }
		void MutantAbandoned(bool value) { m_mutantAbandoned = value; }

		ULONG SemaphoreCount() { return m_semaphoreCount; }
		void SemaphoreCount(ULONG value) { m_semaphoreCount = value; }

		ULONG SemaphoreLimit() { return m_semaphoreLimit; }
		void SemaphoreLimit(ULONG value) { m_semaphoreLimit = value; }

		ULONG64 SectionBaseAddress() { return m_sectionBaseAddress; }
		void SectionBaseAddress(ULONG64 value) { m_sectionBaseAddress = value; }

		ULONG64 SectionMaximumSize() { return m_sectionMaximumSize; }
		void SectionMaximumSize(ULONG64 value) { m_sectionMaximumSize = value; }

		ULONG SectionAttributes() { return m_sectionAttributes; }
		void SectionAttributes(ULONG value) { m_sectionAttributes = value; }

		ULONG64 TimerRemainingTime() { return m_timerRemainingTime; }
		void TimerRemainingTime(ULONG64 value) { m_timerRemainingTime = value; }

		bool TimerState() { return m_timerState; }
		void TimerState(bool value) { m_timerState = value; }

		LONG IoCompletionDepth() { return m_ioCompletionDepth; }
		void IoCompletionDepth(LONG value) { m_ioCompletionDepth = value; }

	private:
		hstring m_name{ L"" };
		hstring m_path{ L"" };
		hstring m_type{ L"" };
		bool m_permanent{ false };
		ULONG m_references{ 0 };
		ULONG m_handles{ 0 };
		ULONG m_pagedPool{ 0 };
		ULONG m_nonPagedPool{ 0 };
		hstring m_creationTime{ L"" };
		hstring m_link{ L"" };
		hstring m_eventType{ L"" };
		bool m_eventSignaled{ false };
		ULONG m_mutantHoldCount{ 0 };
		bool m_mutantAbandoned{ false };
		ULONG m_semaphoreCount{ 0 };
		ULONG m_semaphoreLimit{ 0 };
		ULONG64 m_sectionBaseAddress{ 0 };
		ULONG64 m_sectionMaximumSize{ 0 };
		ULONG m_sectionAttributes{ 0 };
		ULONG64 m_timerRemainingTime{ 0 };
		bool m_timerState{ false };
		LONG m_ioCompletionDepth{ 0 };
	};
}

namespace winrt::StarlightGUI::factory_implementation
{
	struct ObjectEntry : ObjectEntryT<ObjectEntry, implementation::ObjectEntry>
	{
	};
}
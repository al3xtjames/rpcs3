﻿#include "stdafx.h"
#include "sys_vm.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Memory/vm_locking.h"

sys_vm_t::sys_vm_t(u32 _addr, u32 vsize, lv2_memory_container* ct, u32 psize)
	: ct(ct)
	, psize(psize)
	, addr(_addr)
	, size(vsize)
{
	// Write ID
	g_ids[addr >> 28].release(idm::last_id());
}

sys_vm_t::~sys_vm_t()
{
	// Free ID
	g_ids[addr >> 28].release(id_manager::id_traits<sys_vm_t>::invalid);

	// Free block
	verify(HERE), vm::unmap(addr);

	// Return memory
	ct->used -= psize;
}

LOG_CHANNEL(sys_vm);

error_code sys_vm_memory_map(ppu_thread& ppu, u32 vsize, u32 psize, u32 cid, u64 flag, u64 policy, vm::ptr<u32> addr)
{
	vm::temporary_unlock(ppu);

	sys_vm.error("sys_vm_memory_map(vsize=0x%x, psize=0x%x, cid=0x%x, flags=0x%llx, policy=0x%llx, addr=*0x%x)", vsize, psize, cid, flag, policy, addr);

	if (!vsize || !psize || vsize % 0x2000000 || vsize > 0x10000000 || psize > 0x10000000 || policy != SYS_VM_POLICY_AUTO_RECOMMENDED)
	{
		return CELL_EINVAL;
	}

	const auto idm_ct = idm::get<lv2_memory_container>(cid);

	const auto ct = cid == SYS_MEMORY_CONTAINER_ID_INVALID ? g_fxo->get<lv2_memory_container>() : idm_ct.get();

	if (!ct)
	{
		return CELL_ESRCH;
	}

	if (!ct->take(psize))
	{
		return CELL_ENOMEM;
	}

	// Look for unmapped space
	if (const auto area = vm::find_map(0x10000000, 0x10000000, 2 | (flag & SYS_MEMORY_PAGE_SIZE_MASK)))
	{
		// Alloc all memory (shall not fail)
		verify(HERE), area->alloc(vsize);

		idm::make<sys_vm_t>(area->addr, vsize, ct, psize);

		// Write a pointer for the allocated memory
		*addr = area->addr;
		return CELL_OK;
	}

	ct->used -= psize;
	return CELL_ENOMEM;
}

error_code sys_vm_memory_map_different(ppu_thread& ppu, u32 vsize, u32 psize, u32 cid, u64 flag, u64 policy, vm::ptr<u32> addr)
{
	vm::temporary_unlock(ppu);

	sys_vm.warning("sys_vm_memory_map_different(vsize=0x%x, psize=0x%x, cid=0x%x, flags=0x%llx, policy=0x%llx, addr=*0x%x)", vsize, psize, cid, flag, policy, addr);
	// TODO: if needed implement different way to map memory, unconfirmed.

	return sys_vm_memory_map(ppu, vsize, psize, cid, flag, policy, addr);
}

error_code sys_vm_unmap(ppu_thread& ppu, u32 addr)
{
	vm::temporary_unlock(ppu);

	sys_vm.warning("sys_vm_unmap(addr=0x%x)", addr);

	// Special case, check if its a start address by alignment
	if (addr % 0x10000000)
	{
		return CELL_EINVAL;
	}

	// Free block and info
	if (!idm::remove<sys_vm_t>(sys_vm_t::find_id(addr)))
	{
		return CELL_EINVAL;
	}

	return CELL_OK;
}

error_code sys_vm_append_memory(ppu_thread& ppu, u32 addr, u32 size)
{
	vm::temporary_unlock(ppu);

	sys_vm.warning("sys_vm_append_memory(addr=0x%x, size=0x%x)", addr, size);

	if (!size || size % 0x100000)
	{
		return CELL_EINVAL;
	}

	const auto block = idm::get<sys_vm_t>(sys_vm_t::find_id(addr));

	if (!block || block->addr != addr)
	{
		return CELL_EINVAL;
	}

	std::lock_guard lock(block->mutex);

	if (!block->ct->take(size))
	{
		return CELL_ENOMEM;
	}

	block->psize += size;
	return CELL_OK;
}

error_code sys_vm_return_memory(ppu_thread& ppu, u32 addr, u32 size)
{
	vm::temporary_unlock(ppu);

	sys_vm.warning("sys_vm_return_memory(addr=0x%x, size=0x%x)", addr, size);

	if (!size || size % 0x100000)
	{
		return CELL_EINVAL;
	}

	const auto block = idm::get<sys_vm_t>(sys_vm_t::find_id(addr));

	if (!block || block->addr != addr)
	{
		return CELL_EINVAL;
	}

	std::lock_guard lock(block->mutex);

	if (u64{block->psize} < u64{0x100000} + size)
	{
		return CELL_EBUSY;
	}

	block->psize -= size;
	block->ct->used -= size;
	return CELL_OK;
}

error_code sys_vm_lock(ppu_thread& ppu, u32 addr, u32 size)
{
	vm::temporary_unlock(ppu);

	sys_vm.warning("sys_vm_lock(addr=0x%x, size=0x%x)", addr, size);

	if (!size)
	{
		return CELL_EINVAL;
	}

	const auto block = idm::get<sys_vm_t>(sys_vm_t::find_id(addr));

	if (!block || u64{addr} + size > u64{block->addr} + block->size)
	{
		return CELL_EINVAL;
	}

	return CELL_OK;
}

error_code sys_vm_unlock(ppu_thread& ppu, u32 addr, u32 size)
{
	vm::temporary_unlock(ppu);

	sys_vm.warning("sys_vm_unlock(addr=0x%x, size=0x%x)", addr, size);

	if (!size)
	{
		return CELL_EINVAL;
	}

	const auto block = idm::get<sys_vm_t>(sys_vm_t::find_id(addr));

	if (!block || u64{addr} + size > u64{block->addr} + block->size)
	{
		return CELL_EINVAL;
	}

	return CELL_OK;
}

error_code sys_vm_touch(ppu_thread& ppu, u32 addr, u32 size)
{
	vm::temporary_unlock(ppu);

	sys_vm.warning("sys_vm_touch(addr=0x%x, size=0x%x)", addr, size);

	if (!size)
	{
		return CELL_EINVAL;
	}

	const auto block = idm::get<sys_vm_t>(sys_vm_t::find_id(addr));

	if (!block || u64{addr} + size > u64{block->addr} + block->size)
	{
		return CELL_EINVAL;
	}

	return CELL_OK;
}

error_code sys_vm_flush(ppu_thread& ppu, u32 addr, u32 size)
{
	vm::temporary_unlock(ppu);

	sys_vm.warning("sys_vm_flush(addr=0x%x, size=0x%x)", addr, size);

	if (!size)
	{
		return CELL_EINVAL;
	}

	const auto block = idm::get<sys_vm_t>(sys_vm_t::find_id(addr));

	if (!block || u64{addr} + size > u64{block->addr} + block->size)
	{
		return CELL_EINVAL;
	}

	return CELL_OK;
}

error_code sys_vm_invalidate(ppu_thread& ppu, u32 addr, u32 size)
{
	vm::temporary_unlock(ppu);

	sys_vm.warning("sys_vm_invalidate(addr=0x%x, size=0x%x)", addr, size);

	if (!size)
	{
		return CELL_EINVAL;
	}

	const auto block = idm::get<sys_vm_t>(sys_vm_t::find_id(addr));

	if (!block || u64{addr} + size > u64{block->addr} + block->size)
	{
		return CELL_EINVAL;
	}

	return CELL_OK;
}

error_code sys_vm_store(ppu_thread& ppu, u32 addr, u32 size)
{
	vm::temporary_unlock(ppu);

	sys_vm.warning("sys_vm_store(addr=0x%x, size=0x%x)", addr, size);

	if (!size)
	{
		return CELL_EINVAL;
	}

	const auto block = idm::get<sys_vm_t>(sys_vm_t::find_id(addr));

	if (!block || u64{addr} + size > u64{block->addr} + block->size)
	{
		return CELL_EINVAL;
	}

	return CELL_OK;
}

error_code sys_vm_sync(ppu_thread& ppu, u32 addr, u32 size)
{
	vm::temporary_unlock(ppu);

	sys_vm.warning("sys_vm_sync(addr=0x%x, size=0x%x)", addr, size);

	if (!size)
	{
		return CELL_EINVAL;
	}

	const auto block = idm::get<sys_vm_t>(sys_vm_t::find_id(addr));

	if (!block || u64{addr} + size > u64{block->addr} + block->size)
	{
		return CELL_EINVAL;
	}

	return CELL_OK;
}

error_code sys_vm_test(ppu_thread& ppu, u32 addr, u32 size, vm::ptr<u64> result)
{
	vm::temporary_unlock(ppu);

	sys_vm.warning("sys_vm_test(addr=0x%x, size=0x%x, result=*0x%x)", addr, size, result);

	const auto block = idm::get<sys_vm_t>(sys_vm_t::find_id(addr));

	if (!block || u64{addr} + size > u64{block->addr} + block->size)
	{
		return CELL_EINVAL;
	}

	*result = SYS_VM_STATE_ON_MEMORY;

	return CELL_OK;
}

error_code sys_vm_get_statistics(ppu_thread& ppu, u32 addr, vm::ptr<sys_vm_statistics_t> stat)
{
	vm::temporary_unlock(ppu);

	sys_vm.warning("sys_vm_get_statistics(addr=0x%x, stat=*0x%x)", addr, stat);

	const auto block = idm::get<sys_vm_t>(sys_vm_t::find_id(addr));

	if (!block || block->addr != addr)
	{
		return CELL_EINVAL;
	}

	stat->page_fault_ppu = 0;
	stat->page_fault_spu = 0;
	stat->page_in = 0;
	stat->page_out = 0;
	stat->pmem_total = block->psize;
	stat->pmem_used = 0;
	stat->timestamp = 0;

	return CELL_OK;
}

DECLARE(sys_vm_t::g_ids){};

#pragma once
#include "etna.hh"
#include <cstdint>

// =============================================================================
//  etna_arena.hh -- a bump allocator for per-batch GPU data
// =============================================================================
//
// WHY THIS IS NOT OPTIONAL once draws are batched.
//
// A single-draw-per-submission caller can upload vertices into one buffer,
// submit, and wait -- the GPU is finished with the buffer before the CPU
// touches it again. As soon as several draws share one submission that stops
// being true: the FE reads vertex buffers asynchronously right up until the
// fence retires, so re-uploading into the same buffer between draws races the
// GPU and renders nondeterministic garbage.
//
// So every draw in a batch needs vertex memory that nobody touches until the
// batch's fence. This hands out non-overlapping slices of one pool and is reset
// only after the fence -- which is the whole contract.
//
// Slices are plain `Bo`s pointing into the pool (the identity map makes that a
// simple base+offset), so cpu_prep/cpu_fini work on them normally.

namespace etna
{

class Arena {
public:
	// Carve `bytes` out of the GPU pool. Call once.
	bool init(Gpu &gpu, uint32_t bytes)
	{
		pool_ = gpu.alloc(bytes, 64);
		used_ = 0;
		return bool(pool_);
	}

	// A slice of `bytes`, or an empty Bo (operator bool == false) when the
	// arena is exhausted -- callers must check, since silently overlapping
	// slices would be a memory-corruption bug rather than a visible failure.
	Bo alloc(uint32_t bytes, uint32_t align = 64)
	{
		const uint32_t start = (used_ + align - 1) & ~(align - 1);
		if (!pool_ || start + bytes > pool_.bytes)
			return {};
		used_ = start + bytes;
		return Bo{.phys = pool_.phys + start, .bytes = bytes, .cacheable = pool_.cacheable};
	}

	// Convenience: allocate and upload in one step, leaving the data clean in
	// DDR ready for the GPU to read.
	template<typename T>
	Bo upload(std::span<const T> data, uint32_t align = 64)
	{
		Bo bo = alloc(static_cast<uint32_t>(data.size_bytes()), align);
		if (!bo)
			return bo;
		auto dst = bo.span<T>();
		for (size_t i = 0; i < data.size(); i++)
			dst[i] = data[i];
		bo.cpu_fini(RelocWrite);
		return bo;
	}

	// ONLY after the batch's fence has retired -- see the note above.
	void reset()
	{
		used_ = 0;
	}

	uint32_t used() const
	{
		return used_;
	}
	uint32_t capacity() const
	{
		return pool_.bytes;
	}

private:
	Bo pool_{};
	uint32_t used_ = 0;
};

} // namespace etna

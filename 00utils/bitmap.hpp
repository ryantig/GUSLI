#pragma once
#include <stdint.h>		// uint64_t, uint32_t and such
/******************************* Bit operations ******************************/
class bitmap {
 protected:
	uint64_t *bmp;	// Pointer to allocated array of bits
	uint32_t n_bits;	// Amount of bits in bitmap

	static inline uint32_t __fls(uint64_t x) {
		uint32_t num = BITS_PER_LONG - 1;
		if (!(x & (~0ul <<                  32 ))) { num -= 32; x <<= 32; }
		if (!(x & (~0ul << (BITS_PER_LONG - 16)))) { num -= 16; x <<= 16; }
		if (!(x & (~0ul << (BITS_PER_LONG -  8)))) { num -=  8; x <<=  8; }
		if (!(x & (~0ul << (BITS_PER_LONG -  4)))) { num -=  4; x <<=  4; }
		if (!(x & (~0ul << (BITS_PER_LONG -  2)))) { num -=  2; x <<=  2; }
		if (!(x & (~0ul << (BITS_PER_LONG -  1)))) { num -=  1; x <<=  1; }
		return num;
	}
	static inline uint32_t fls64(uint64_t x) {	// fls = Find last set bit,  returns 0 if value is 0 or the position of the last set bit if value is nonzero. The last (most significant) bit is at position 64
		if (x == 0)	return 0;
		return __fls(x) + 1;
	}
	static inline uint32_t ffs64(uint64_t x) {	// ffs = Find first set bit
		uint32_t num = 0;
		if ((x & 0xffffffff) == 0) { num += 32; x >>= 32; }
		if ((x & 0xffff    ) == 0) { num += 16; x >>= 16; }
		if ((x & 0xff      ) == 0) { num +=  8; x >>=  8; }
		if ((x & 0xf       ) == 0) { num +=  4; x >>=  4; }
		if ((x & 0x3       ) == 0) { num +=  2; x >>=  2; }
		if ((x & 0x1       ) == 0)   num += 1;
		return num;
	}
 public:
 	// Static utils
	static constexpr uint32_t BITS_PER_LONG = 64;
	static uint64_t BIT_MASK( int nr) { return (1UL << (nr % BITS_PER_LONG)); }
	static uint64_t BIT_WORD( int nr) { return         (nr / BITS_PER_LONG); }
	static uint64_t BITMAP_FIRST_WORD_MASK(int start) { return (~0UL << (  start  & (BITS_PER_LONG - 1))); }
	static uint64_t BITMAP_LAST_WORD_MASK( int nbits) { return (~0UL >> ((-nbits) & (BITS_PER_LONG - 1))); }
	static inline int hweight64(uint64_t x)	{return __builtin_popcountll(x); }

 	bitmap(uint64_t& p0, uint32_t _n_bits) : bmp(&p0), n_bits(_n_bits) {};
	inline uint32_t size( void) const { return n_bits; }
	inline void set_bit(  int nr) { bmp[BIT_WORD(nr)] |=  BIT_MASK(nr); }
	inline void clear_bit(int nr) { bmp[BIT_WORD(nr)] &= ~BIT_MASK(nr); }
	inline bool test_bit( int nr) const { return 1UL & (bmp[BIT_WORD(nr)] >> (nr & (BITS_PER_LONG-1))); }
	inline uint32_t find_first_bit(void) const {
		for (uint32_t i = 0; i * BITS_PER_LONG < n_bits; i++) {
			if (bmp[i]) return min(i * BITS_PER_LONG + ffs64(bmp[i]), n_bits);
		}
		return n_bits;
	}
	inline uint32_t find_next_bit(uint32_t start, uint64_t invert = 0UL) const {
		if (unlikely(start >= n_bits))
			return n_bits;
		uint64_t cur = bmp[start / BITS_PER_LONG] ^ invert;
		cur   &= BITMAP_FIRST_WORD_MASK(start);		// Handle 1st partial u64.
		start &= ~(BITS_PER_LONG - 1);				// Multiple of 64
		while (!cur) {
			start += BITS_PER_LONG;
			if (start >= n_bits)
				return n_bits;
			cur = bmp[start / BITS_PER_LONG] ^ invert;
		}
		return min(start + ffs64(cur), n_bits);
	}
	inline uint64_t find_next_zero_bit(uint64_t start) const { return find_next_bit(start, ~0UL); }
	uint32_t find_next_bit_circular(uint32_t pos) const {
		uint32_t rv = find_next_bit(pos + 1);
		return (rv == n_bits) ? find_first_bit() : rv;
	}
	void set_all(void) {
		const uint32_t last_u64 = (n_bits-1) / BITS_PER_LONG;
		if (last_u64) memset(bmp, 0xFF, last_u64 * sizeof(uint64_t));
		bmp[last_u64] = BITMAP_LAST_WORD_MASK(n_bits);
	}
	void clear_all(void) { memset(bmp, 0x0, ((n_bits + BITS_PER_LONG - 1) / BITS_PER_LONG) * sizeof(uint64_t)); 	}
	int  get_bmp_length(void) const { return n_bits; }
};
#define for_each_set_bit(     b, bmp) for ((b) = bmp.find_first_bit(); (b) < bmp.size(); (b) =  bmp.find_next_bit((b) + 1))
#define for_each_set_bit_from(b, bmp) for ((b) = bmp.find_next_bit(b); (b) < bmp.size(); (b) =  bmp.find_next_bit((b) + 1))

template <int N_bits>
class small_ints_set : public bitmap {		// Stores a bit field, turned on bit for element in a set
	uint64_t arr[(N_bits + bitmap::BITS_PER_LONG - 1) / bitmap::BITS_PER_LONG] = {0};
 public:
 	small_ints_set(void) : bitmap(arr[0], N_bits) {}
 	small_ints_set(int _n_bits) : bitmap(arr[0], _n_bits) { DEBUG_ASSERT(_n_bits <= N_bits); }
	void insert(uint32_t x) { ASSERT_IN_PRODUCTION(x < size()); set_bit(x); }
	void remove(uint32_t x) { clear_bit(x); }
};
class cpu_mask_set : public bitmap {
	uint64_t arr[4] = {0, 0, 0, 0};		// Support up to 256 cpus
 public:
	cpu_mask_set(int _n_bits = 0) : bitmap(arr[0], _n_bits) { BUG_ON(_n_bits > (int)(sizeof(arr) * 8), "CPU mask object is too small");}
	const cpu_mask_set &operator=(const cpu_mask_set& n) noexcept {
		nvTODO("Support working with more than 64 cores!\n");
		memcpy(arr, n.arr, sizeof(arr));
		this->n_bits = n.n_bits;
		this->bmp = this->arr;
		return *this;
	}
	uint16_t get_n_total_cores(void) const { return get_bmp_length(); } // Use this if you want to alloc for each core.
	bool is_empty(void) const { return (arr[0]|arr[1]|arr[2]|arr[3]) == 0UL; }
	static constexpr const unsigned TO_STRING_BUF_LEN = sizeof(arr)*2 + 32;		// Print nibbles in hex with prefix
	int to_string(char buf[TO_STRING_BUF_LEN]) const {
		int n = 0;
		n += snprintf(&buf[n], TO_STRING_BUF_LEN - n, "CPUM=%u:0x", n_bits);
		for (int i = ARRAY_SIZE(arr) - 1; i > 0; --i )
			if (arr[i])
				n += snprintf(&buf[n], TO_STRING_BUF_LEN - n, "%lx,", arr[i]);
		n += snprintf(&buf[n], TO_STRING_BUF_LEN - n, "%lx", arr[0]);
		return n;
	}
	static int get_max_supported_cores(void) { return sizeof(cpu_mask_set::arr) * 8; }
};

namespace pow2 {
	static inline int is_power_of_2(uint32_t x) { return (x != 0) && ((x & (x - 1)) == 0); }
	static inline uint32_t smallest_pow2_no_less_then(uint32_t x) {
		x--;
		x |= x >> 1;
		x |= x >> 2;
		x |= x >> 4;
		x |= x >> 8;
		x |= x >> 16;
		return x + 1;
	}
};

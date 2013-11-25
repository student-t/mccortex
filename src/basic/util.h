#ifndef UTIL_H_
#define UTIL_H_

// comparison returns:
//   negative iff a < b
//          0 iff a == b
//   positive iff a > b

int cmp_int(const void *a, const void *b);
int cmp_long(const void *a, const void *b);
int cmp_float(const void *a, const void *b);
int cmp_double(const void *a, const void *b);
int cmp_uint32(const void *a, const void *b);
int cmp_uint64(const void *a, const void *b);
int cmp_size(const void *a, const void *b);
int cmp_ptr(const void *a, const void *b);

char parse_entire_int(char *str, int *result);
char parse_entire_uint(char *str, unsigned int *result);
char parse_entire_ulong(char *str, unsigned long *result);
char parse_entire_double(char *str, double *result);
char parse_entire_size(char *str, size_t *result);

// Load a string of numbers into an array. Separator can be any non-numerical
// character. e.g. '1,2,3' '1 2 3'
// Returns number of unsigned integers parsed.
// Sets *more = 1 if string end not reached
uint32_t parse_uint_liststr(const char *str, uint32_t *arr, uint32_t arrlen,
                            boolean *more);

// Get number of integers in a list
uint32_t len_uint_liststr(const char *str);

boolean parse_uint_liststr_strict(const char *str, char sep,
                                  uint32_t *arr, uint32_t arrlen);

size_t count_char(const char *str, char c);

boolean bases_to_integer(const char *arg, size_t *bases);
boolean mem_to_integer(const char *arg, size_t *bytes);


unsigned int num_of_digits(unsigned long num);

// result must be long enough for result + 1 ('\0'). Max length required is:
// strlen('18,446,744,073,709,551,615')+1 = 27 bytes
// returns pointer to result
char* ulong_to_str(unsigned long num, char* result);

// result must be long enough for result + 1 ('\0'). Max length required is:
// strlen('-9,223,372,036,854,775,808')+1 = 27 bytes
char* long_to_str(long num, char* result);

// result must be long enough for result + 1 ('\0').
// Max length required is: 26+1+decimals+1 = 28+decimals bytes
// strlen('-9,223,372,036,854,775,808') = 27
// strlen('.') = 1
// +1 for \0
char* double_to_str(double num, int decimals, char* str);

// str must be 26 + 3 + 1 + num decimals + 1 = 31+decimals bytes
// breakdown:
// strlen('18,446,744,073,709,551,615') = 26
// strlen(' GB') = 3
// strlen('.') = 1
// +1 for '\0'
char* bytes_to_str(unsigned long num, int decimals, char* str);

// Number to string using G to mean 10^9, M to mean 10^6 etc
char* num_to_str(unsigned long num, int decimals, char* str);

//
// Bits and binary
//

// Unaligned memory access
#define unaligned_read(tgt,src)            memcpy(tgt, src, sizeof(tgt))
// #define unaligned_read(tgt,src,ptrtype) (*(tgt) = *(ptrtype)(src))

#define bitmask(nbits,type) (nbits ? ~(type)0 >> (sizeof(type)*8-(nbits)): (type)0)

#define rot32(x,r) (((x)<<(r)) | ((x)>>(32-(r))))
#define rot64(x,r) (((x)<<(r)) | ((x)>>(64-(r))))

// A nibble is 4 bits (i.e. half a byte)
#define rev_nibble(x) ((((x)&1)<<3)|(((x)&2)<<1)|(((x)&4)>>1)|(((x)&8)>>3))

#define round_bits_to_bytes(bits)   (((bits)+7)/8)
#define round_bits_to_words64(bits) (((bits)+63)/64)

#define bitset2_has(arr,idx,offset)    (((arr)[idx] >> (offset)) & 0x1)
#define bitset2_set(arr,idx,offset)     ((arr)[idx] |= (0x1UL << (offset)))
#define bitset2_del(arr,idx,offset)     ((arr)[idx] &=~(0x1UL << (offset)))
#define bitset2_cpy(arr,idx,offset,bit) ((arr)[idx] |= ((uint64_t)(bit) << (offset)))

#define bitset_has(arr,pos) \
        bitset2_has(arr, (pos)/(sizeof(*(arr))*8), (pos)%(sizeof(*(arr))*8))
#define bitset_set(arr,pos) \
        bitset2_set(arr, (pos)/(sizeof(*(arr))*8), (pos)%(sizeof(*(arr))*8))
#define bitset_del(arr,pos) \
        bitset2_del(arr, (pos)/(sizeof(*(arr))*8), (pos)%(sizeof(*(arr))*8))
#define bitset_cpy(arr,pos,bit) \
        bitset2_cpy(arr, (pos)/(sizeof(*(arr))*8), (pos)%(sizeof(*(arr))*8), (bit))

#define bitset_clear_word(arr,pos) ((arr)[(pos) / (sizeof(*(arr))*8)] = 0)


//
// Maths
//

float log_factorial(unsigned int number);
float log_factorial_ll(unsigned long long number);
unsigned long calculate_mean_ulong(unsigned long *array, unsigned long len);

//
// Time
//

// output of form: "10 days 23 hours 59 mins 59 secs"
// extreme: "-2147483647 days 23 hours 59 mins 59 secs",
// so str should be at least 42 bytes long
// returns number of bytes written
size_t seconds_to_str(unsigned long seconds, char *str);

//
// Genetics
//

extern const char complement_base[128];

#ifdef NDEBUG
  #define char_nucleotide_complement(c) complement_base[(int)(c)]
#else
  char char_nucleotide_complement(char c);
#endif

#define char_is_dna_base(c) (char_to_bnuc[(int)(c)] != UndefinedBase)

// length is the length in number of bases
// the char* should have one MORE base than that allocated, to hold '\0'
char *reverse_complement_str(char *str, size_t length);

#endif /* UTIL_H_ */

#include <locale.h>

#include <utf.h>

@ {
    #include <uchar.h>
    #include <stdint.h>
}

/**
  Get the code point from a UTF-8 character, starting at *str.
  THIS FUNCTION IS UNSAFE, strings passed to this should be sanitized,
  otherwise security issues could occur in code using this function.
*/
@ uint32_t utf8_codepoint(const char* str) {
    if (!(str[0] & 0x80)) return (uint32_t) str[0]; /* ASCII */
    if ((str[0] & 0xE0) == 0xC0) /* & 1110 0000 == 1100 0000 */
        return (uint32_t) ((str[0] & 0x1F) << 6) |
            (uint32_t) ((str[1] & 0x3F) << 0);
    if ((str[0] & 0xF0) == 0xE0) /* & 1111 0000 == 110 0000 */
        return (uint32_t) ((str[0] & 0x0F) << 12) |
            (uint32_t) ((str[1] & 0x3F) << 6) |
            (uint32_t) ((str[2] & 0x3F));
    if ((str[0] & 0xF8) == 0xF0) /* & 1111 1000 == 1111 0000 */
        return (uint32_t) ((str[0] & 0x07) << 18) |
            (uint32_t) ((str[1] & 0x3F) << 12) |
            (uint32_t) ((str[2] & 0x3F) << 6) |
            (uint32_t) ((str[3] & 0x3F) << 0);
    return (uint32_t) ' ';
}

/** Returns the size of the next UTF-8 character in a string */
@ size_t utf8_next(const char* str) {
    if (!(str[0] & 0x80)) return 1;
    if ((str[0] & 0xE0) == 0xC0) return 2;
    if ((str[0] & 0xF0) == 0xE0) return 3;
    if ((str[0] & 0xF8) == 0xF0) return 4;
    return 0; /* invalid encoding, or str pointer is not offset correctly */
}

@ {
    #include <stdint.h>
    /*
      Iterate over a C char string 'str', setting the start of each UTF-8
      sequence to 'p'. Will loop forever on invalid strings.
    */
    #define utf8_iter_unsafe(str, p)                    \
        typeof(str) p;                                  \
        for (p = str; *p != '\0'; p += utf8_next(p))

    /*
      Same as above, but breaks on invalid character start and stores the
      size of the current character in 'n'.
    */
    #define utf8_iter(str, p, n)                                \
        typeof(str) p;                                          \
        size_t n = utf8_next(str);                              \
        for (p = str; n && *p != '\0'; p += (n = utf8_next(p)))
    
    static inline size_t utf8_len(const char* str) {
        size_t count = 0;
        utf8_iter(str, _, n) {
            ++count;
        }
        return count;
    }
}

/* Pointer arrays must always include the array size, because pointers do not know about the size of
   the supposed array size. */
// src: https://gist.github.com/tommai78101/3631ed1f136b78238e85582f08bdc618
// probably not a good idea to just rip this but this is an annoying function to implement
@ void utf8_to_utf16(const char* utf8_str, int utf8_str_size,
                          char16_t* utf16_str_output, int utf16_str_output_size) {
	/* First, grab the first byte of the UTF-8 string */
	const unsigned char* utf8_currentCodeUnit = (const unsigned char*) utf8_str;
	char16_t* utf16_currentCodeUnit = utf16_str_output;
	int utf8_str_iterator = 0;
	int utf16_str_iterator = 0;

	/* In a while loop, we check if the UTF-16 iterator is less than the max output size. If true, then we
       check if UTF-8 iterator is less than UTF-8 max string size. This conditional checking based on order
       of precedence is intentionally done so it prevents the while loop from continuing onwards if the
       iterators are outside of the intended sizes. */
	while (*utf8_currentCodeUnit && (utf16_str_iterator < utf16_str_output_size
                                     || utf8_str_iterator < utf8_str_size)) {
		/* Figure out the current code unit to determine the range. It is split into 6 main groups,
           each of which handles the data differently from one another. */
		if (*utf8_currentCodeUnit < 0x80) {
			/* 0..127, the ASCII range. */

			/* We directly plug in the values to the UTF-16 code unit. */
			*utf16_currentCodeUnit = (char16_t) (*utf8_currentCodeUnit);
			utf16_currentCodeUnit++;
			utf16_str_iterator++;
			/* Increment the current code unit pointer to the next code unit */
			utf8_currentCodeUnit++;
			/* Increment the iterator to keep track of where we are in the UTF-8 string */
			utf8_str_iterator++;
		}
		else if (*utf8_currentCodeUnit < 0xC0) {
			/* 0x80..0xBF, we ignore. These are reserved for UTF-8 encoding. */
			utf8_currentCodeUnit++;
			utf8_str_iterator++;
		}
		else if (*utf8_currentCodeUnit < 0xE0) {
			/* 128..2047, the extended ASCII range, and into the Basic Multilingual Plane. */

			/* Work on the first code unit. */
			char16_t highShort = (char16_t) ((*utf8_currentCodeUnit) & 0x1F);
			/* Increment the current code unit pointer to the next code unit */
			utf8_currentCodeUnit++;
			/* Work on the second code unit. */
			char16_t lowShort = (char16_t) ((*utf8_currentCodeUnit) & 0x3F);
			/* Increment the current code unit pointer to the next code unit */
			utf8_currentCodeUnit++;
			/* Create the UTF-16 code unit, then increment the iterator */
			int unicode = (highShort << 8) | lowShort;

			/* Check to make sure the "unicode" is in the range [0..D7FF] and [E000..FFFF]. */
			if ((0 <= unicode && unicode <= 0xD7FF) || (0xE000 <= unicode && unicode <= 0xFFFF)) {
				//Directly set the value to the UTF-16 code unit.
				*utf16_currentCodeUnit = (char16_t) unicode;
				utf16_currentCodeUnit++;
				utf16_str_iterator++;
			}

			/* Increment the iterator to keep track of where we are in the UTF-8 string */
			utf8_str_iterator += 2;
		}
		else if (*utf8_currentCodeUnit < 0xF0) {
			/* 2048..65535, the remaining Basic Multilingual Plane. */

			/* Work on the UTF-8 code units one by one. */
			/* If drawn out, it would be 1110aaaa 10bbbbcc 10ccdddd */
			/* Where a is 4th byte, b is 3rd byte, c is 2nd byte, and d is 1st byte. */
			char16_t fourthChar = (char16_t) ((*utf8_currentCodeUnit) & 0xF);
			utf8_currentCodeUnit++;
			char16_t thirdChar = (char16_t) ((*utf8_currentCodeUnit) & 0x3C) >> 2;
			char16_t secondCharHigh = (char16_t) ((*utf8_currentCodeUnit) & 0x3);
			utf8_currentCodeUnit++;
			char16_t secondCharLow = (char16_t) ((*utf8_currentCodeUnit) & 0x30) >> 4;
			char16_t firstChar = (char16_t) ((*utf8_currentCodeUnit) & 0xF);
			utf8_currentCodeUnit++;

			/* Create the resulting UTF-16 code unit, then increment the iterator. */
			int unicode = (fourthChar << 12) | (thirdChar << 8) | (secondCharHigh << 6)
                | (secondCharLow << 4) | firstChar;

			/* Check to make sure the "unicode" is in the range [0..D7FF] and [E000..FFFF]. */
			/* According to math, UTF-8 encoded "unicode" should always fall within these two ranges. */
			if ((0 <= unicode && unicode <= 0xD7FF) || (0xE000 <= unicode && unicode <= 0xFFFF)) {
				//Directly set the value to the UTF-16 code unit.
				*utf16_currentCodeUnit = (char16_t) unicode;
				utf16_currentCodeUnit++;
				utf16_str_iterator++;
			}

			/* Increment the iterator to keep track of where we are in the UTF-8 string */
			utf8_str_iterator += 3;
		}
		else if (*utf8_currentCodeUnit < 0xF8) {
			/* 65536..10FFFF, the Unicode UTF range */

			/* Work on the UTF-8 code units one by one.
               If drawn out, it would be 11110abb 10bbcccc 10ddddee 10eeffff
               Where a is 6th byte, b is 5th byte, c is 4th byte, and so on. */
			char16_t sixthChar = (char16_t) ((*utf8_currentCodeUnit) & 0x4) >> 2;
			char16_t fifthCharHigh = (char16_t) ((*utf8_currentCodeUnit) & 0x3);
			utf8_currentCodeUnit++;
			char16_t fifthCharLow = (char16_t) ((*utf8_currentCodeUnit) & 0x30) >> 4;
			char16_t fourthChar = (char16_t) ((*utf8_currentCodeUnit) & 0xF);
			utf8_currentCodeUnit++;
			char16_t thirdChar = (char16_t) ((*utf8_currentCodeUnit) & 0x3C) >> 2;
			char16_t secondCharHigh = (char16_t) ((*utf8_currentCodeUnit) & 0x3);
			utf8_currentCodeUnit++;
			char16_t secondCharLow = (char16_t) ((*utf8_currentCodeUnit) & 0x30) >> 4;
			char16_t firstChar = (char16_t) ((*utf8_currentCodeUnit) & 0xF);
			utf8_currentCodeUnit++;

			int unicode = (sixthChar << 4) | (fifthCharHigh << 2) | fifthCharLow |
                (fourthChar << 12) | (thirdChar << 8) | (secondCharHigh << 6)
                | (secondCharLow << 4) | firstChar;
			char16_t highSurrogate = (unicode - 0x10000) / 0x400 + 0xD800;
			char16_t lowSurrogate = (unicode - 0x10000) % 0x400 + 0xDC00;

			/* Set the UTF-16 code units */
			*utf16_currentCodeUnit = lowSurrogate;
			utf16_currentCodeUnit++;
			utf16_str_iterator++;

			/* Check to see if we're still below the output string size before continuing,
               otherwise, we cut off here. */
			if (utf16_str_iterator < utf16_str_output_size) {
				*utf16_currentCodeUnit = highSurrogate;
				utf16_currentCodeUnit++;
				utf16_str_iterator++;
			}

			/* Increment the iterator to keep track of where we are in the UTF-8 string */
			utf8_str_iterator += 4;
		}
		else {
			/* Invalid UTF-8 code unit, we ignore. */
			utf8_currentCodeUnit++;
			utf8_str_iterator++;
		}
	}

	/* We clean up the output string if the UTF-16 iterator is still less than the output string size. */
	while (utf16_str_iterator < utf16_str_output_size) {
		*utf16_currentCodeUnit = '\0';
		utf16_currentCodeUnit++;
		utf16_str_iterator++;
	}
}

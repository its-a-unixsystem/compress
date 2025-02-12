#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <inttypes.h>

/* --- Constants and Type Definitions --- */

#define ID_DICT_T uint16_t
#define ENDOFDICTIONARY "ENDOFDICTIONARY"
#define LINEEND "\r\n"

#define PRICETYPE int32_t
#define SPRICETYPE int16_t
#define MANTISSA int8_t

#define MAX_LINE_LENGTH 1000
#define CSV_BUFFER_SIZE 1024
#define RECORD_SIZE 5  /* fixed record size for decompression */

/// Global debug flag (set via command-line option -x)
static bool debug = false;

typedef struct {
    PRICETYPE integer;  // For money, no floats
    MANTISSA mantissa;  // The position at which to insert a decimal point
} price_t;

typedef struct {
    char *ticker;           // Ticker string (dynamically allocated)
    unsigned char exchange;
    char side;
    char condition;
    unsigned char flags;    /* Bit flags:
                               Bit0, Bit1, Bit2: Side encoding 
                               Bit3: sendtime == recvtime 
                               Bit4: sendtime stored as a diff to previous 
                               Bit5: exchange same as previous 
                               Bit6: size stored as 2 bytes (small) 
                               Bit7: price stored as 2 bytes (small) 
                            */
    uint32_t sendtime;
    uint8_t sendtimediff;
    uint32_t recvtime;
    price_t price;
    uint32_t size;
} TradeRecord_t;

typedef struct ticker_dict {
    ID_DICT_T frequency;
    ID_DICT_T entry;
    char *symbol;
    struct ticker_dict *next;
} ticker_dict_t;

/* --- Bit Manipulation Helpers --- */

static inline uint8_t set_bit(uint8_t flags, int bit) {
    return flags | (1 << bit);
}

static inline bool is_bit_set(uint8_t flags, int bit) {
    return (flags & (1 << bit)) != 0;
}

/* --- String Helpers --- */

/**
 * shift_string_right
 * Shifts the substring starting at position pos in str to the right by shift characters.
 */
static void shift_string_right(char *str, int pos, int shift) {
    size_t len = strlen(str);
    memmove(str + pos + shift, str + pos, len - pos + 1);  // +1 to include the null terminator
}

/* --- Price Conversion Functions --- */

/**
 * price_to_string
 *
 * Converts a price_t value into its string representation.
 * This function allocates memory which must be freed by the caller.
 */
char* price_to_string(price_t price) {
    // Determine required buffer size:
    // Start with the length needed for the integer portion.
    int int_len = snprintf(NULL, 0, "%" PRId32, price.integer);
    int extra = abs(price.mantissa) + 3; // extra room for '.', possible sign, and '\0'
    size_t buf_size = int_len + extra;
    char *buf = malloc(buf_size);
    if (!buf) {
        perror("malloc failed in price_to_string");
        exit(EXIT_FAILURE);
    }
    snprintf(buf, buf_size, "%" PRId32, price.integer);

    int offset = (price.integer < 0) ? 1 : 0;
    
    if (price.mantissa < 0) {
        int shift_amount = abs(price.mantissa) + 1;
        shift_string_right(buf, 0, shift_amount);
        for (int i = offset; i < abs(price.mantissa) + offset + 1; i++) {
            buf[i] = '0';
        }
        price.mantissa = abs(price.mantissa);
        memmove(buf + price.mantissa + 1, buf + price.mantissa, strlen(buf) - price.mantissa + 1);
    } else {
        memmove(buf + price.mantissa + 1, buf + price.mantissa, strlen(buf) - price.mantissa + 1);
    }
    buf[price.mantissa + offset] = '.';

    // Special-case handling
    if (buf[0] == '.') {
        shift_string_right(buf, 0, 1);
        buf[0] = '0';
    } else if (strncmp(buf, "-.", 2) == 0) {
        shift_string_right(buf, 1, 1);
        buf[0] = '-';
        buf[1] = '0';
    } else if (buf[strlen(buf) - 1] == '.') {
        buf[strlen(buf) - 1] = '\0';
    }
    
    if (strncmp(buf, "00.", 3) == 0) {
        buf[0] = '0';
        buf[1] = '.';
        buf[2] = '0';
    } else if (strncmp(buf, "-00.", 4) == 0) {
        buf[0] = '-';
        buf[1] = '0';
        buf[2] = '.';
        buf[3] = '0';
    }
    
    if (strcmp(buf, "0.0") == 0) {
        strcpy(buf, "0");
    }
    
    return buf;
}

/**
 * parse_price_from_string
 *
 * Parses a string representing a price (e.g. "123.45") into a price_t value.
 * Uses a local copy so that the original string is not modified.
 */
price_t parse_price_from_string(const char *price_str) {
    price_t price;
    char buffer[256];
    strncpy(buffer, price_str, sizeof(buffer) - 1);
    buffer[sizeof(buffer)-1] = '\0';
    
    char *pos_decimal = strchr(buffer, '.');
    if (!pos_decimal) {
        pos_decimal = buffer + strlen(buffer);
    }
    price.mantissa = (int)(pos_decimal - buffer);
    
    // Remove the decimal point from the string
    memmove(pos_decimal, pos_decimal + 1, strlen(pos_decimal + 1) + 1);
    
    price.integer = atoi(buffer);
    int offset = (price.integer < 0) ? 1 : 0;
    
    if (price.integer < 0) {
        price.mantissa--;
    }
    
    if ((buffer[0] == '0') || (price.integer < 0 && buffer[1] == '0')) {
        for (int i = offset; buffer[i] == '0'; i++) {
            price.mantissa--;
        }
    }
    
    return price;
}

/* --- CSV Parsing --- */

/**
 * parse_csv_line
 *
 * Parses a CSV line (assumed to have at least 7 comma–separated fields) and returns
 * a TradeRecord_t structure. The caller is responsible for freeing the allocated ticker string.
 */
TradeRecord_t parse_csv_line(const char *csv_line) {
    TradeRecord_t record;
    char line_copy[CSV_BUFFER_SIZE];
    strncpy(line_copy, csv_line, sizeof(line_copy) - 1);
    line_copy[sizeof(line_copy)-1] = '\0';
    
    char *token;
    char *rest = line_copy;
    
    // Ticker
    token = strtok_r(rest, ",", &rest);
    if (!token) { fprintf(stderr, "CSV parse error: missing ticker\n"); exit(EXIT_FAILURE); }
    record.ticker = strdup(token);
    
    // Exchange, side, condition
    token = strtok_r(NULL, ",", &rest);
    if (!token) { fprintf(stderr, "CSV parse error: missing exchange\n"); exit(EXIT_FAILURE); }
    record.exchange = token[0];
    
    token = strtok_r(NULL, ",", &rest);
    if (!token) { fprintf(stderr, "CSV parse error: missing side\n"); exit(EXIT_FAILURE); }
    record.side = token[0];
    
    token = strtok_r(NULL, ",", &rest);
    if (!token) { fprintf(stderr, "CSV parse error: missing condition\n"); exit(EXIT_FAILURE); }
    record.condition = token[0];
    
    // Initialize flags to 0 and set side encoding flags.
    record.flags = 0;
    switch(record.side) {
        case 'A':
            record.flags = set_bit(record.flags, 0);
            break;
        case 'a':
            record.flags = set_bit(record.flags, 1);
            break;
        case 'B':
            record.flags = set_bit(record.flags, 0);
            record.flags = set_bit(record.flags, 1);
            break;
        case 'b':
            record.flags = set_bit(record.flags, 2);
            break;
        case 'T':
            record.flags = set_bit(record.flags, 0);
            record.flags = set_bit(record.flags, 2);
            break;
        default:
            break;
    }
    
    // Parse times
    token = strtok_r(NULL, ",", &rest);
    if (!token) { fprintf(stderr, "CSV parse error: missing sendtime\n"); exit(EXIT_FAILURE); }
    record.sendtime = (uint32_t)atoi(token);
    
    token = strtok_r(NULL, ",", &rest);
    if (!token) { fprintf(stderr, "CSV parse error: missing recvtime\n"); exit(EXIT_FAILURE); }
    record.recvtime = (uint32_t)atoi(token);
    
    if (record.sendtime == record.recvtime) {
        record.flags = set_bit(record.flags, 3);
    }
    
    // Parse price and size (price comes first)
    token = strtok_r(NULL, ",", &rest);
    if (!token) { fprintf(stderr, "CSV parse error: missing price\n"); exit(EXIT_FAILURE); }
    record.price = parse_price_from_string(token);
    
    token = strtok_r(NULL, ",", &rest);
    if (!token) { fprintf(stderr, "CSV parse error: missing size\n"); exit(EXIT_FAILURE); }
    record.size = (uint32_t)atoi(token);
    
    return record;
}

/* --- Dictionary (Linked List) Functions --- */

/**
 * new_dict_list_entry
 *
 * Creates a new ticker_dict_t node with the given symbol and next pointer.
 * The Dictionary counter (passed by pointer) is used to assign the node’s entry ID.
 */
ticker_dict_t* new_dict_list_entry(const char *symbol, ticker_dict_t *next, ID_DICT_T *dictionary_counter) {
    ticker_dict_t *node = malloc(sizeof(ticker_dict_t));
    if (!node) {
        perror("Failed to allocate dictionary list entry");
        exit(EXIT_FAILURE);
    }
    node->symbol = strdup(symbol);
    if (!node->symbol) {
        perror("Failed to allocate symbol");
        exit(EXIT_FAILURE);
    }
    node->frequency = 1;
    node->entry = (*dictionary_counter);
    node->next = next;
    
    (*dictionary_counter)++;
    return node;
}

/**
 * add_dict_list
 *
 * Adds a new node (with the given symbol) to the head of the dictionary list.
 */
ticker_dict_t* add_dict_list(const char *symbol, ticker_dict_t *list, ID_DICT_T *dictionary_counter) {
    return new_dict_list_entry(symbol, list, dictionary_counter);
}

/**
 * increment_dict_list_entry
 *
 * Searches for the given symbol in the list. If found, increments its frequency and returns its entry ID.
 */
ID_DICT_T increment_dict_list_entry(const char *symbol, ticker_dict_t *list) {
    ticker_dict_t *current = list;
    while (current != NULL) {
        if (strcmp(symbol, current->symbol) == 0) {
            current->frequency++;
            return current->entry;
        }
        current = current->next;
    }
    return 0;  // Not found; caller should have added it.
}

/**
 * find_dict_list_entry
 *
 * Searches for the given symbol and returns its entry ID.
 */
ID_DICT_T find_dict_list_entry(const char *symbol, ticker_dict_t *list) {
    ticker_dict_t *current = list;
    while (current != NULL) {
        if (strcmp(symbol, current->symbol) == 0) {
            return current->entry;
        }
        current = current->next;
    }
    return 0;
}

/**
 * find_dict_list_symbol
 *
 * Searches for the given entry ID and returns the associated symbol.
 */
char* find_dict_list_symbol(ID_DICT_T entry, ticker_dict_t *list) {
    ticker_dict_t *current = list;
    while (current != NULL) {
        if (entry == current->entry) {
            return current->symbol;
        }
        current = current->next;
    }
    return NULL;
}

/**
 * dict_list_search
 *
 * Returns a pointer to the node with the given symbol (or NULL if not found).
 */
ticker_dict_t* dict_list_search(const char *symbol, ticker_dict_t *list) {
    ticker_dict_t *current = list;
    while (current != NULL) {
        if (strcmp(current->symbol, symbol) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

/**
 * destroy_dict_list
 *
 * Frees the entire dictionary list.
 */
void destroy_dict_list(ticker_dict_t *list) {
    ticker_dict_t *current;
    while (list != NULL) {
        current = list;
        list = list->next;
        free(current->symbol);
        free(current);
    }
}

/**
 * dump_dictionary
 *
 * Writes the dictionary to the provided file handle.
 */
void dump_dictionary(ticker_dict_t *dict, FILE *dict_file) {
    const unsigned char terminator = 0;
    const char dict_end[] = ENDOFDICTIONARY;
    
    ticker_dict_t *current = dict;
    while (current != NULL) {
        fwrite(&current->entry, sizeof(current->entry), 1, dict_file);
        fwrite(current->symbol, strlen(current->symbol), 1, dict_file);
        fwrite(&terminator, sizeof(char), 1, dict_file);
        current = current->next;
    }
    /* Write extra terminators and dictionary end marker */
    fwrite(&terminator, sizeof(terminator), 2, dict_file);
    fwrite(dict_end, sizeof(dict_end), 1, dict_file);
}

/**
 * read_dictionary
 *
 * Reads the dictionary from the given file handle.
 */
ticker_dict_t* read_dictionary(ticker_dict_t *dict, FILE *dict_file) {
    ID_DICT_T number = 0;
    char *line = NULL;
    size_t len = 0;
    ssize_t read_len;
    char *symbol = NULL;
    
    while (fread(&number, sizeof(ID_DICT_T), 1, dict_file) == 1 &&
           (read_len = getdelim(&line, &len, '\0', dict_file)) > 0) {
        symbol = realloc(symbol, strlen(line) + 1);
        if (!symbol) {
            perror("realloc failed in read_dictionary");
            exit(EXIT_FAILURE);
        }
        memcpy(symbol, line, strlen(line));
        symbol[strlen(line)] = '\0';
        
        /* Check for dictionary terminator */
        if (strcmp(symbol, ENDOFDICTIONARY) == 0) {
            break;
        }
        dict = add_dict_list(symbol, dict, &number);
    }
    
    if (errno) {
        perror("Error reading dictionary");
    }
    free(line);
    free(symbol);
    return dict;
}

/* --- Compression Functionality --- */

/**
 * do_compress
 *
 * Reads CSV lines from input_file, builds a dictionary and encodes the records into output_file.
 */
void do_compress(FILE *input_file, FILE *output_file, ticker_dict_t *dict) {
    char line[MAX_LINE_LENGTH];
    TradeRecord_t record;
    ID_DICT_T dictionary_counter = 1;
    ID_DICT_T tmp_dictionary_number = 0;
    FILE *dict_file = NULL;
    
    int64_t time_diff = 0;
    uint32_t last_time = 0;
    uint16_t size_small = 0;
    SPRICETYPE price_small = 0;
    char last_exchange = 0;
    
    /* If debug mode is enabled, write the dictionary to a temporary file */
    if (debug) {
        dict_file = tmpfile();
        if (!dict_file) {
            perror("Error creating temporary dictionary file");
            exit(EXIT_FAILURE);
        }
    } else {
        dict_file = output_file;
    }
    
    printf("Pass 1 - building dictionary\n");
    while (fgets(line, sizeof(line), input_file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';  // Remove line endings
        
        record = parse_csv_line(line);
        
        if (!dict_list_search(record.ticker, dict)) {
            dict = add_dict_list(record.ticker, dict, &dictionary_counter);
        } else {
            increment_dict_list_entry(record.ticker, dict);
        }
        free(record.ticker);
    }
    
    rewind(input_file);
    
    printf("Pass 2 - encoding data\n");
    
    /* Write the dictionary */
    dump_dictionary(dict, dict_file);
    
    while (fgets(line, sizeof(line), input_file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';  // Remove line endings
        record = parse_csv_line(line);
        
        time_diff = record.sendtime - last_time;
        if (abs(time_diff) > 254 || last_time > record.sendtime) {
            record.sendtimediff = 0;
        } else {
            record.sendtimediff = (uint8_t)time_diff;
            record.flags = set_bit(record.flags, 4);
        }
        
        if (last_exchange == record.exchange) {
            record.flags = set_bit(record.flags, 5);
        }
        
        if (record.size < 65534) {
            size_small = (uint16_t)record.size;
            record.flags = set_bit(record.flags, 6);
        }
        
        if (abs(record.price.integer) < 32767) {
            price_small = (SPRICETYPE)record.price.integer;
            record.flags = set_bit(record.flags, 7);
        }
        
        tmp_dictionary_number = find_dict_list_entry(record.ticker, dict);
        
        /* Write fixed record fields: ticker ID, condition, flags, mantissa */
        fwrite(&tmp_dictionary_number, sizeof(ID_DICT_T), 1, output_file);
        fwrite(&record.condition, sizeof(record.condition), 1, output_file);
        fwrite(&record.flags, sizeof(record.flags), 1, output_file);
        fwrite(&record.price.mantissa, sizeof(record.price.mantissa), 1, output_file);
        
        /* Write price: small (2 bytes) or large (4 bytes) */
        if (is_bit_set(record.flags, 7)) {
            fwrite(&price_small, sizeof(price_small), 1, output_file);
        } else {
            fwrite(&record.price.integer, sizeof(record.price.integer), 1, output_file);
        }
        
        /* Write size: small (2 bytes) or large (4 bytes) */
        if (is_bit_set(record.flags, 6)) {
            fwrite(&size_small, sizeof(size_small), 1, output_file);
        } else {
            fwrite(&record.size, sizeof(record.size), 1, output_file);
        }
        
        /* Write exchange if it differs from the previous record */
        if (!is_bit_set(record.flags, 5)) {
            fwrite(&record.exchange, sizeof(record.exchange), 1, output_file);
        }
        
        /* Write send time or time difference */
        if (is_bit_set(record.flags, 4)) {
            fwrite(&record.sendtimediff, sizeof(record.sendtimediff), 1, output_file);
        } else {
            fwrite(&record.sendtime, sizeof(record.sendtime), 1, output_file);
        }
        
        /* Write recv time if not equal to send time */
        if (!is_bit_set(record.flags, 3)) {
            fwrite(&record.recvtime, sizeof(record.recvtime), 1, output_file);
        }
        
        free(record.ticker);
        last_exchange = record.exchange;
        last_time = record.sendtime;
    }
}

/**
 * do_decompress
 *
 * Reads compressed data from input_file, decodes it (using the stored dictionary) and writes CSV lines to output_file.
 */
void do_decompress(FILE *input_file, FILE *output_file, ticker_dict_t *dict) {
    size_t bytes_read;
    char input_data[RECORD_SIZE];
    void *cursor;
    TradeRecord_t record;
    ID_DICT_T entry_id;
    char *symbol;
    uint16_t size_small = 0;
    char last_exchange = 0;
    SPRICETYPE price_small = 0;
    uint32_t last_time = 0;
    
    printf("Decompressing...\n");
    
    /* Read the dictionary from the file */
    dict = read_dictionary(dict, input_file);
    
    while ((bytes_read = fread(input_data, RECORD_SIZE, 1, input_file)) == 1) {
        memcpy(&entry_id, input_data, sizeof(ID_DICT_T));
        cursor = input_data + sizeof(ID_DICT_T);
        
        memcpy(&record.condition, cursor, sizeof(record.condition));
        cursor += sizeof(record.condition);
        
        memcpy(&record.flags, cursor, sizeof(record.flags));
        cursor += sizeof(record.flags);
        
        memcpy(&record.price.mantissa, cursor, sizeof(record.price.mantissa));
        cursor += sizeof(record.price.mantissa);
        
        /* Decode the side from the flags */
        if (is_bit_set(record.flags, 0) && !is_bit_set(record.flags, 1) && !is_bit_set(record.flags, 2)) {
            record.side = 'A';
        } else if (!is_bit_set(record.flags, 0) && is_bit_set(record.flags, 1) && !is_bit_set(record.flags, 2)) {
            record.side = 'a';
        } else if (is_bit_set(record.flags, 0) && is_bit_set(record.flags, 1) && !is_bit_set(record.flags, 2)) {
            record.side = 'B';
        } else if (!is_bit_set(record.flags, 0) && !is_bit_set(record.flags, 1) && is_bit_set(record.flags, 2)) {
            record.side = 'b';
        } else if (is_bit_set(record.flags, 0) && !is_bit_set(record.flags, 1) && is_bit_set(record.flags, 2)) {
            record.side = 'T';
        }
        
        /* Read price (small or large) */
        if (is_bit_set(record.flags, 7)) {
            bytes_read = fread(input_data, 2, 1, input_file);
            memcpy(&price_small, input_data, sizeof(price_small));
            record.price.integer = price_small;
        } else {
            bytes_read = fread(input_data, 4, 1, input_file);
            memcpy(&record.price.integer, input_data, sizeof(record.price.integer));
        }
        char *price_str = price_to_string(record.price);
        
        /* Read size (small or large) */
        if (is_bit_set(record.flags, 6)) {
            bytes_read = fread(input_data, 2, 1, input_file);
            memcpy(&size_small, input_data, sizeof(size_small));
            record.size = size_small;
        } else {
            bytes_read = fread(input_data, 4, 1, input_file);
            memcpy(&record.size, input_data, sizeof(record.size));
        }
        
        /* Read exchange (either same as previous or stored explicitly) */
        if (is_bit_set(record.flags, 5)) {
            record.exchange = last_exchange;
        } else {
            bytes_read = fread(input_data, 1, 1, input_file);
            memcpy(&record.exchange, input_data, sizeof(record.exchange));
        }
        
        /* Read send time (either full timestamp or a diff) */
        if (is_bit_set(record.flags, 4)) {
            bytes_read = fread(input_data, 1, 1, input_file);
            memcpy(&record.sendtimediff, input_data, sizeof(record.sendtimediff));
            record.sendtime = last_time + record.sendtimediff;
        } else {
            bytes_read = fread(input_data, 4, 1, input_file);
            memcpy(&record.sendtime, input_data, sizeof(record.sendtime));
        }
        
        /* Read recv time if different */
        if (is_bit_set(record.flags, 3)) {
            record.recvtime = record.sendtime;
        } else {
            bytes_read = fread(input_data, 4, 1, input_file);
            memcpy(&record.recvtime, input_data, sizeof(record.recvtime));
        }
        
        symbol = find_dict_list_symbol(entry_id, dict);
        if (!symbol) {
            fprintf(stderr, "Symbol not found for entry %u\n", entry_id);
            exit(EXIT_FAILURE);
        }
        
        fprintf(output_file, "%s,%c,%c,%c,%u,%u,%s,%u%s",
                symbol,
                record.exchange,
                record.side,
                record.condition,
                record.sendtime,
                record.recvtime,
                price_str,
                record.size,
                LINEEND);
        
        free(price_str);
        last_time = record.sendtime;
        last_exchange = record.exchange;
    }
}

/* --- Main --- */

int main (int argc, char **argv) {
    bool compress = true;  /* default mode: compress */
    char *input_filename = NULL;
    char *output_filename = NULL;
    FILE *input_file = NULL, *output_file = NULL;
    ticker_dict_t *ticker_dict = NULL;
    int opt;
    
    /* Parse command-line options */
    opterr = 0;
    while ((opt = getopt(argc, argv, "cdx")) != -1) {
        switch (opt) {
            case 'c':
                compress = true;
                break;
            case 'd':
                compress = false;
                break;
            case 'x':
                debug = true;
                break;
            case '?':
                if (isprint(optopt))
                    fprintf(stderr, "Unknown option `-%c'.\n", optopt);
                else
                    fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
                return EXIT_FAILURE;
            default:
                abort();
        }
    }
    
    if (argc - optind != 2) {
        fprintf(stderr, "Usage: compress [-c|-d|-x] <inputfile> <outputfile>\n");
        exit(EXIT_FAILURE);
    }
    
    input_filename = argv[optind];
    output_filename = argv[optind+1];
    
    input_file = fopen(input_filename, "r");
    if (!input_file) {
        perror("Error opening input file");
        exit(EXIT_FAILURE);
    }
    
    output_file = fopen(output_filename, "w+");  /* Overwrite existing file */
    if (!output_file) {
        perror("Error opening output file");
        fclose(input_file);
        exit(EXIT_FAILURE);
    }
    
    if (compress) {
        printf("Compressing %s into %s\n", input_filename, output_filename);
        do_compress(input_file, output_file, ticker_dict);
    } else {
        printf("Decompressing %s into %s\n", input_filename, output_filename);
        do_decompress(input_file, output_file, ticker_dict);
    }
    
    destroy_dict_list(ticker_dict);
    
    fclose(input_file);
    fclose(output_file);
    
    return EXIT_SUCCESS;
}

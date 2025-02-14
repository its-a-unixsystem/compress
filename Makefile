# Simple Makefile for compress/decompress
CC      = gcc
CFLAGS  = -Wall -Wextra -O2
TARGET  = compress
SRC     = compress.c
OBJ     = $(SRC:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# A simple test:
# 1. Create a minimal CSV input file.
# 2. Compress it to a binary file.
# 3. Decompress it back to CSV.
# 4. Compare the original CSV to the decompressed CSV.
test: $(TARGET)
	@echo "AAPL,N,A,?,123456789,123456789,123.45,100" > test_input.csv
	@echo "Running compression..."
	./$(TARGET) -c test_input.csv test_output.bin
	@echo "Running decompression..."
	./$(TARGET) -d test_output.bin test_output.csv
	@echo "Comparing original and decompressed files..."
	@diff test_input.csv test_output.csv && \
	  echo "Test passed!" || echo "Test failed!"

clean:
	rm -f $(TARGET) $(OBJ) test_input.csv test_output.bin test_output.csv

.PHONY: all test clean

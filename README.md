# compress
CME compression exercise

##Objective:
Write a program that compresses a file of the following format, by taking advantage of patterns in the data.

The file is in bid/ask/trade tick file format (attached file). BAT format is a CSV linefeed row-delimited text file defined as follows:

- ticker: variable-length string
- exchange: a single character indicating the exchange posting the event
- side: a single character indicating type of event (B/b/A/a/T, indicating BestBid, Bid, BestAsk, Ask, Trade)
- condition: a character indicating type of quote or trade
- sendtime: time at which event occurred on exchange, in milliseconds-after-midnight format
- recvtime: time at which the event was received on the local host
- price: price on the event, in USD
- size: number of shares on the event

###Example:
```
IBM,N,T,0,30612216,30612247,98.8,115600
IBM,N,b,O,30612315,30612347,98.78,1000
IBM,N,a,O,30612315,30612347,98.89,1000
IBM,N,B,0,30612315,30612347,98.78,1000
IBM,N,b,R,30612315,30612347,98.75,700
IBM,N,B,0,30612315,30612348,98.75,700
IBM,N,a,R,30612315,30612348,99.03,1500
```

Each row in the file comprises a tick. The objective is to

1. implement code that compresses the file into another file containing the same data, but making the new storage more compact
2. implement code that extracts  the data. The data should be identical with the original input file
3. The application should be a command line application with one file name for input and one file name for output.

These files are typically several tens of gigabytes in size, so while the attached file is small, this can be taken into account when determining a good compression algorithm. Dictionary data will for instance typically be dwarfed.

**Note:**  Don't use any 3rd party or framework provided compression libraries for your solution.  We also ask that you only use libraries provided with the base compiler (e.g. no Boost).  The solution should be based on your own code.

##Deliverables:

1. Implement the project to use the following command line:  
**Compress.exe [option] inputFile outputFile**
111Compress.exe – name of your solution
inputFile – path to the input file
outputFile = path to the output file111
Where [option] is –c or –d  (-c = compress, -d = decompress)
2. The source code for the project
3. Metrics demonstrating the compression of the program. Compression numbers should break out any meta/dictionary information and present compression both with and without this data.
4. Documentation on the algorithm and implementation

# answer

Overview
========

The code is written in C. I chose it because I wanted to be able to fully control sizes, though some segfaults and mallocs later I almost regretted it ;-) 
It uses some GNU specialities like getopt, so it is not strictly ANSI/C99 but should be fairly portable. I only tested under a Ubuntu 17.10 system.

Compile with 
```gcc -Wall compress.c -o compress```

It understands the following options:
```  compress [-c|-d|-x] <inputfile> <outputfile>```

-x enables the debug mode, in which the dictionary is not written.

Compression ratio:
==================

Original:
```
>wc -c cmeebat.csv
19113524 cmeebat.csv
```

With dictionary:
```
>wc -c outfile.dat
5217222 outfile.dat
```

RATIO: ==> 1:3.663 or 72.70%

Without dictionary:
```
>wc -c outfile-debug.dat
5208981 outfile-debug.dat
```

RATIO: ==> 1:3.669 or 72.75%

LZ77 comparision:
```
>wc -c cmeebat.csv.gz 
4102227 cmeebat.csv.gz
```

RATIO: ==> 1:4.659 or 78.54%

Implementation:
==============

Due to some design decisions (like the chosen language) I did not have the time to implement everything I wanted to:
 * Especially the code to deal with the integer/mantissa breakdown and re-assembling took too much time, has likely some undiscovered edge cases and would need a cleanup/rewrite. Under real-world conditions I would have used a 3rd party library (such as mathlib) instead or coding it myself, therefore the compression is better data encoding.
 * I would need to add return checks throughout the code, there is little to none error recognition. Also, it makes Valgrind cry.
 * I would need to refactor/clean up some code, especially in do_compress & do_decompress are way too long.
 * I currently read the input file twice, this can be optimised, by putting the dictionary at the end of the file
 * One could use only a subset of the bits in a field (ie. using only 7bits in strings and reusing the 8th bit for flags).
 * During the build-up of the dictionary I wanted to search for min/max values to size the fields.

Assumptions
-----------
A few assumptions I took from the sample data, leading to design decisions:
 * send & receive times are often the same
 * the price and size often fits into a 2byte field
 * the exchange flag is often the same
 * the difference between the times are often very small (ie.<255 mseconds)

The compression ration depends heavily on those assumptions.

Ticker dictionary
-----------------

I use a dictionary for the ticker encoding, which is a simple list. With more time I would have changed it to a Huffman tree, the frequency is already collected. The dictionary is written at the beginning of the compressed file in the following format (each square is 1 byte):
```
[Y][Y] dictionary ID
[X][X]...[X] ticker string
[\0] null terminator
```
The dictionary ends when a ticker named "ENDOFDICTIONARY" is seen.

Records
-------

There is a fixed record for each entry, with a additional record if needed. The fixed record is (1 square = 1 byte):
```
[A][A]                - dictionary ID
[B]                   - condition value
[C]                   - record flags/bitfield
[D]                   - mantissa part of the price
```
Optional fields are
```
[E][E] or [E][E][E][E] - integer part of the price
[F][F] or [F][F][F][F] - size of trade
[G]                    - exchange
[H]    or [H][H]       - send time, diff or full
[I][I]                 - recvtime
```
the flags [C] are bit encoded and mean:
```
  bit 0-2 - "side" of the trade
  bit 3   - if     set, the recvtime is the same as sendtime
               not set there will be an [I] with the recvtime
  bit 4   - if     set, the sendtime is [H] 1 byte diff to the previous one
               not set the full sendtime [H][H] is added
  bit 5   - if     set, the exchange is the same as the previous one
               not set the full exchange [G] is added
  bit 6   - if     set, the size is in a 2byte field [E][E]
               not set the size is in a 4byte field [E][E][E][E]
  bit 7   - if     set, the price is in a 2byte field [E][E]
               not set the price is in a 4byte field [E][E][E][E]
```
So the minimum record size is 10 bytes:
```
[A][A][B][C][D][E][E][F][F][H]
```

maximum is 18 bytes:
```
[A][A][B][C][D][E][E][E][E][F][F][F][F][G][H][H][I][I]
```

Limitations
-----------
There are some assumptions I made regarding the data:
 * maximum dictionary entries: 65535, (ID_DICT_T)
 * maximum CSV line size: 1000, (MAXLINELENGTH)
 * 255 maximum digits of the price (MANTISSA+string length)
 * maximum integer part of the price +/- 2147483647 (PRICETYPE)
 * no ticker can be named ENDOFDICTIONARY

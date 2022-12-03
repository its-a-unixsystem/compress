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

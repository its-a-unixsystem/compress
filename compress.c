#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <inttypes.h>


/* maximum dictionary size 65k entries... */
#define ID_DICT_T uint16_t

#define ENDOFDICTIONARY "ENDOFDICTIONARY"

#define LINEEND "\r\n"

#define PRICETYPE int32_t
#define SPRICETYPE int16_t
#define MANTISSA int8_t

/* what is a safe assumtion a maximum csv line is? */
#define MAXLINELENGTH 1000

#define RECORDSIZE 5 /* minimum record */

typedef uint8_t bool;
#define true 1
#define false 0

typedef struct price_t
{
  PRICETYPE integer; // seen max 12733, 4bytes, no float for money...
  MANTISSA mantissa; // seen 0.2265625, 2bytes...
} price_t;

typedef struct TradeRecord_t
{
  char *ticker;           //  ticker string
  unsigned char exchange;
  char side;
  char condition;
  unsigned char flags; /*
			 0 -\
			 1   +- side encoding
			 2 -/
			 3 - sendtime=recvtime
			 4 - sendtime is a diff to previous
			 5 - exchange is same as previous
			 6 - if set "size" is 4byte, otherwise 2byte
			 7 - if set "price" is 4byte, otherwise 2byte
			*/
  uint32_t sendtime; //              uint:4294967295, maxsec/day: 86400000
  uint8_t sendtimediff; //           max time diff=255
  uint32_t recvtime;
  price_t price;
  uint32_t size;
} TradeRecord_t;

typedef struct ticker_dict_t
{
  ID_DICT_T frequency;
  ID_DICT_T entry;
  char *symbol;
  struct ticker_dict_t* next;
} ticker_dict_t;

/* global var if we run in debug mode */
bool debug = false;

void usage()
{
  printf("compress [-c|-d|-x] <inputfile> <outputfile>\n");
}

// shift string <buf> from position <pos> on <num> positions to the right
char* strshiftr(char* buf,int pos,int num)
{
  int i;
  for(i=1;i<=num;i++)
    {
      memmove(&buf[pos+1],&buf[pos],sizeof(&buf));      
    }

  return buf;
}

// convert the structure <price> to a string
char* parse_price_out(price_t price,char* return_buf)
{
  size_t memory_needed;
  int i, offset,len;

  memory_needed=snprintf(NULL, 0, "%" PRId32, price.integer);

  // we are more than generous to account for -,. and \0
  memory_needed=memory_needed+abs(price.mantissa)+3;


  return_buf=malloc(memory_needed);
  sprintf(return_buf, "%" PRId32, price.integer);

  // account for - sign
  offset=(price.integer<0?1:0);

  // negative mantissa means shifting the number to the right,
  // filling with zeros
  if(price.mantissa<0)
    {
      return_buf=strshiftr(return_buf,0,abs(price.mantissa)+1);
      
      for(i=offset;i!=abs(price.mantissa)+offset+1;i++)
	{
	  return_buf[i]='0';
	}

      price.mantissa=abs(price.mantissa);

      memmove(&return_buf[price.mantissa+1],
       	      &return_buf[price.mantissa],
       	      strlen(return_buf)-price.mantissa);
    }
  else
    {
      memmove(&return_buf[price.mantissa+1],
	      &return_buf[price.mantissa],
	      strlen(return_buf)-price.mantissa+1);
    }

  return_buf[price.mantissa+offset]='.';

  // we account for special cases
  
  if(return_buf[0]=='.') // single dot in front
    {
      len=strlen(return_buf);
      return_buf=strshiftr(return_buf,0,1);
      return_buf[0]='0';
      return_buf[len+1]='\0';
    }
  else if(strncmp(&return_buf[0],"-.",2)==0) // minus-dot in front
    {
      len=strlen(return_buf);
      return_buf=strshiftr(return_buf,1,1);
      return_buf[0]='-';
      return_buf[1]='0';
      return_buf[len+1]='\0';
    }
  else if(return_buf[strlen(return_buf)-1]=='.') // trailing dot
    {
      return_buf[strlen(return_buf)-1]='\0';
    }  

  // edge cases not caught, TODO/FIXME
  if(strncmp(&return_buf[0],"00.",3)==0)
    {
      return_buf[0]='0';
      return_buf[1]='.';
      return_buf[2]='0'; 
    }
  else if(strncmp(&return_buf[0],"-00.",4)==0)
    {
      return_buf[0]='-';
      return_buf[1]='0';
      return_buf[2]='.';
      return_buf[3]='0'; 
    }

  // output format for single 0 is 0
  // above logic produces 0.0 though
  if(strcmp(return_buf,"0.0")==0)
    {
      strcpy(return_buf,"0");
    }

  return return_buf;
}

// we parse a string into integer+mantissa
price_t parse_price_in(char buf[256])
{
  char* pos_decimal;
  price_t price;
  int i,offset;


  pos_decimal=strstr(buf, ".");
  if(pos_decimal==NULL)
    {
      pos_decimal=&buf[(strlen(buf))];
    }
  
  price.mantissa=(int)(pos_decimal-buf); // length-position of .
  
  // shift
  memmove(&buf[pos_decimal-buf],
	  &buf[pos_decimal-buf+1],
	  strlen(buf)-(int)(pos_decimal-buf));

  price.integer=atoi(buf);
  offset=(price.integer<0?1:0);
  
  if(price.integer<0) // account for negative sign
    {
      price.mantissa--;
    }

  // if number starts with a 0 or -0
  // move the mantissa
  if((buf[0]=='0') || (buf[1]=='0' && price.integer<0))
    {
      for(i=offset;buf[i]=='0';i++)
	{
	  price.mantissa--;
	}
    }
  
  return price; 
}

// bitfield manipulations
char setBit(char bit,char byte)
{
  byte |= (1 << bit);
  return byte;
}

bool getBit(char bit,char byte)
{
  bool ret;
  ret=(byte & (1 << bit));
  return  ret;
}

// parse a CSV line into a structure
TradeRecord_t parse_CSV(char CSVLineIn[1024])
{
  char* tmp_token;
  
  struct TradeRecord_t DataRecord;
  
  tmp_token=strtok(CSVLineIn, ",");
  DataRecord.ticker=strdup(tmp_token);
  
  // get exchange, side, condition
  DataRecord.exchange = strtok(NULL, ",")[0]; // only the first byte
  DataRecord.side = strtok(NULL, ",")[0];
  DataRecord.condition = strtok(NULL, ",")[0];

  // encode some flags
  DataRecord.flags=0;
  
  if(DataRecord.side=='A')
    {
      DataRecord.flags=setBit(0,DataRecord.flags);
    }
  else if(DataRecord.side=='a')
    {
      DataRecord.flags=setBit(1,DataRecord.flags);
    }
  else if(DataRecord.side=='B')
    {
      DataRecord.flags=setBit(0,DataRecord.flags);
      DataRecord.flags=setBit(1,DataRecord.flags);      
    }
  else if(DataRecord.side=='b')
    {
      DataRecord.flags=setBit(2,DataRecord.flags);
    }
  else if(DataRecord.side=='T')
    {
      DataRecord.flags=setBit(0,DataRecord.flags);
      DataRecord.flags=setBit(2,DataRecord.flags);
    }

  // times
  DataRecord.sendtime = atoi(strtok(NULL, ","));
  DataRecord.recvtime = atoi(strtok(NULL, ","));
  
  if(DataRecord.sendtime == DataRecord.recvtime)
    {
      DataRecord.flags=setBit(3,DataRecord.flags);
    }
  
  // we parse price later, lets not confuse strtok
  tmp_token=strtok(NULL, ",");
  DataRecord.size = atoi(strtok(NULL, ","));

  DataRecord.price = parse_price_in(tmp_token);

  return DataRecord;
}

// list helper functions

// create a new node
ticker_dict_t* NewDictListEntry(char* symbol, ticker_dict_t* next, ID_DICT_T* DictionaryNumber)
{
  ticker_dict_t* node = malloc(sizeof(ticker_dict_t));
  if(node == NULL)
    {
      perror("Oh-oh, error creating new list item.\n");
      exit(-1);
    }
  node->symbol = malloc(strlen(symbol)+1);
  strcpy(node->symbol,symbol);
  node->frequency = 1;
  node->entry = *DictionaryNumber;
  node->next = next;

  (*DictionaryNumber)++;

  return node;
}

// wrapper / TODO
ticker_dict_t* AddDictList(char* symbol, ticker_dict_t* ListStart,ID_DICT_T* DictionaryNumber)
{
  ticker_dict_t* node = NewDictListEntry(symbol,ListStart,DictionaryNumber);
  ListStart = node;

  return ListStart;
}

// increase count for frequency
ID_DICT_T IncDictListEntry(char* symbol, ticker_dict_t* ListStart)
{
  ticker_dict_t *pointer = ListStart;
  
  while(pointer!=NULL)
    {
      if(strcmp(symbol,pointer->symbol)==0)
	{
	  pointer->frequency++;
	  return pointer->entry;
	}
        pointer = pointer->next;
    }
  return ListStart->entry;
}

// search for symbol, return ID
ID_DICT_T FindDictListEntry(char* symbol, ticker_dict_t* ListStart)
{
  ticker_dict_t *pointer = ListStart;
  
  while(pointer!=NULL)
    {
      if(strcmp(symbol,pointer->symbol)==0)
	{
	  return pointer->entry;
	}
        pointer = pointer->next;
    }
  
  return 0;
}

// search for ID, return symbol
char* FindDictListSymbol(ID_DICT_T entry, ticker_dict_t* ListStart)
{
  ticker_dict_t *pointer = ListStart;


  while(pointer!=NULL)
    {
      if(entry==pointer->entry)
	{
	  return pointer->symbol;
	}
        pointer = pointer->next;
    }
  
  return NULL;
}

// look up if symbol already exists
// TODO merge with FindDictListEntry
ticker_dict_t* DictListSearch(char* symbol,ticker_dict_t* ListStart)
{
 
  ticker_dict_t *pointer = ListStart;
  while(pointer!=NULL)
    {
      if(!strcmp(pointer->symbol,symbol))
	{
	  return pointer;
	}
        pointer = pointer->next;
    }
    return NULL;
}

// free space
void DestroyDictList(ticker_dict_t *ListStart)
{
    ticker_dict_t *pointer, *tmp_list;
 
    if(ListStart != NULL)
    {
      pointer = ListStart->next;
      ListStart->next = NULL;
      while(pointer != NULL)
        {
	  tmp_list = pointer->next;
	  free(pointer->symbol);
	  free(pointer);
	  pointer = tmp_list;
        }
    }
}

// write the dictionary to dictfd
void DumpDictionary(ticker_dict_t* ticker_dictionary_start, FILE* dictfd)
{
  ticker_dict_t* pointer = ticker_dictionary_start;
  unsigned char terminator=0;

  char dictend[]=ENDOFDICTIONARY;
  

  while(pointer != NULL)
    {
      fwrite(&pointer->entry,
	     sizeof(pointer->entry),
	     1,
	     dictfd);
      
      fwrite(pointer->symbol,
	     strlen(pointer->symbol),
	     1,
	     dictfd); // strlen = w/o terminator
      
      fwrite(&terminator,
	     sizeof(char),
	     1,
	     dictfd); // we are adding now manually...
      
      pointer = pointer->next;
    }

  /* we just reuse var terminator, number not important */
  fwrite(&terminator,2,1,dictfd);
  fwrite(&dictend,sizeof(dictend),1,dictfd);
}

// read a dictionary from dictfd
ticker_dict_t* ReadDictionary(ticker_dict_t* ticker_dictionary_start, FILE* dictfd)
{
  ID_DICT_T number=0;

  char* line = NULL;
  char* symbol=NULL;
  size_t len,read;

  // TODO check read
  while(fread(&number,sizeof(ID_DICT_T),1,dictfd) &&
	(read=getdelim(&line,&len,0x0000,dictfd)))
    {
      symbol=realloc(symbol,strlen(line)+1);
      memcpy(symbol,line,(size_t)strlen(line));

      // end of dictionary
      if(strcmp(symbol,ENDOFDICTIONARY)==0)
	{
	  break;
	}
      symbol[strlen(line)]='\0';
      
      ticker_dictionary_start=AddDictList(symbol,
					  ticker_dictionary_start,
					  &number);
    }
  
  if(errno)
    perror("reading dictionary");
  
  free(line);
  free(symbol);

  return ticker_dictionary_start;
}

// main encoding function
void do_compress(FILE *inputfh,FILE *outputfh,ticker_dict_t* ticker_dictionary_start)
{

  char line[MAXLINELENGTH]; /* max line length */
  struct TradeRecord_t RecordLine;
  ID_DICT_T DictionaryNumber=1;
  ID_DICT_T TmpDictionaryNumber=0;
  FILE *dictfh;
    
  int64_t timediff=0;
  uint32_t last_time=0;
  uint16_t SizeSmall=0;
  SPRICETYPE PriceSmall=0;
  char last_exchange=0;


  // is we run in debug mode we write the dictionary to a temp file
  // which will get automatically deleted
  if(debug)
    {
      dictfh = tmpfile();
      if(dictfh == NULL)
	{
	  perror("Error opening/creating dictionary file");
	  exit(-1);
	}
    }
  else
    {
      dictfh=outputfh;
    }

  // Pass 1
  printf("Pass 1 - building dictionary\n");
  
  while (fgets(line, sizeof(line), inputfh) != NULL)
  {
    line[strcspn(line, "\r\n")] = 0; // remove line end(s)
    
    RecordLine=parse_CSV(line);

    // if we don't know the ticker yet, we add it to the list
    // otherwise increase the frequency
    if(!DictListSearch(RecordLine.ticker,ticker_dictionary_start))
      {
	ticker_dictionary_start=AddDictList(RecordLine.ticker,
					    ticker_dictionary_start,
					    &DictionaryNumber);
      }
    else
      {
	IncDictListEntry(RecordLine.ticker,ticker_dictionary_start);
      }
    
    free(RecordLine.ticker);
  }

  rewind(inputfh);
  
  printf("Pass 2 - encoding data\n");

  // write dictionary
  DumpDictionary(ticker_dictionary_start,dictfh);

  while (fgets(line, sizeof(line), inputfh) != NULL)
  {
    line[strcspn(line, "\r\n")] = 0; // remove line end(s)

    RecordLine=parse_CSV(line);

    // if the difference to the previous time is small enough
    // use a small value
    timediff=RecordLine.sendtime-last_time;
    if(abs(timediff)>254 || last_time>RecordLine.sendtime)
      {
	RecordLine.sendtimediff=0;
      }
    else
      {
	RecordLine.sendtimediff=timediff;
	RecordLine.flags=setBit(4,RecordLine.flags);
      }

    // same exchange as previous?
    if(last_exchange==RecordLine.exchange)
      {
	RecordLine.flags=setBit(5,RecordLine.flags);
      }

    // small size?
    if(RecordLine.size<65534)
      {
	SizeSmall=RecordLine.size;
	RecordLine.flags=setBit(6,RecordLine.flags);
      }

    // small price?
    if(abs(RecordLine.price.integer)<32767)
      {
	PriceSmall=RecordLine.price.integer;
	RecordLine.flags=setBit(7,RecordLine.flags);
      }

    // look up the ID for the ticker
    TmpDictionaryNumber=FindDictListEntry(RecordLine.ticker,
					  ticker_dictionary_start);

    // write ID, condition, flags and the mantissa
    fwrite(&TmpDictionaryNumber,
	   sizeof(ID_DICT_T),
	   1,
	   outputfh); /* Ticker 16bit 0-65535 2b*/
    
    fwrite(&RecordLine.condition,
	   sizeof(RecordLine.condition),
	   1,outputfh); /* condition 8bit 1b */
    
    fwrite(&RecordLine.flags,
	   sizeof(RecordLine.flags),
	   1,
	   outputfh); /* flags 8bit 1b */
    
    fwrite(&RecordLine.price.mantissa,
	   sizeof(RecordLine.price.mantissa),
	   1,
	   outputfh); /* mantissa 8bit 1b*/

    // write extra data or small/large data
    
    if(getBit(7,RecordLine.flags))
      {
	fwrite(&PriceSmall,
	       sizeof(PriceSmall),
	       1,
	       outputfh); /* 16bit 2b*/
      }
    else
      {
	fwrite(&RecordLine.price.integer,
	       sizeof(RecordLine.price.integer),
	       1,
	       outputfh); /* 32bit 4b*/    	
      }
    
    if(getBit(6,RecordLine.flags))
      {
	fwrite(&SizeSmall,
	       sizeof(SizeSmall),
	       1,
	       outputfh); /* 16bit 2b*/
      }
    else
      {
	fwrite(&RecordLine.size,
	       sizeof(RecordLine.size),
	       1,
	       outputfh); /* 32bit 4b*/
      }

    if(!getBit(5,RecordLine.flags))
      {
	fwrite(&RecordLine.exchange,
	       sizeof(RecordLine.exchange),
	       1,
	       outputfh); /* exchange 8bit */
      }
       
    if(getBit(4,RecordLine.flags))
      {
	fwrite(&RecordLine.sendtimediff,
	       sizeof(RecordLine.sendtimediff),
	       1,
	       outputfh); /* sendtimediff 8bit */
      }
    else
      {
	fwrite(&RecordLine.sendtime,
	       sizeof(RecordLine.sendtime),
	       1,
	       outputfh); /* sendtime 32bit */
      }
    
    if(!getBit(3,RecordLine.flags))
      {
	fwrite(&RecordLine.recvtime,
	       sizeof(RecordLine.recvtime),
	       1,
	       outputfh); /* sendtime 32bit */
      }
    free(RecordLine.ticker);
    last_exchange=RecordLine.exchange;
    last_time=RecordLine.sendtime;
  }

  
}

// main decrompression routine
void do_decompress(FILE *inputfh,FILE *outputfh,ticker_dict_t* ticker_dictionary_start)
{
  
  size_t read;
  char input_data[RECORDSIZE];
  void *cursor;
  TradeRecord_t RecordLine;
  ID_DICT_T* entry=malloc(sizeof(ID_DICT_T));
  char* symbol;
  uint16_t SizeSmall=0;
  char last_exchange=0;
  SPRICETYPE PriceSmall=0;
  
  uint32_t last_time=0;
  
  printf("decompressing\n");

  // read the dictionary
  ticker_dictionary_start=ReadDictionary(ticker_dictionary_start, inputfh);


  // read first the fixed record
  // copy the data into the vars and process some flags
  while((read=fread(&input_data,RECORDSIZE,1,inputfh)))
    {
      memcpy(entry,input_data,sizeof(ID_DICT_T));
      cursor=input_data+sizeof(ID_DICT_T);
      
      memcpy(&RecordLine.condition,cursor,sizeof(RecordLine.condition));
      cursor+=sizeof(RecordLine.condition);    

      memcpy(&RecordLine.flags,cursor,sizeof(RecordLine.flags));
      cursor+=sizeof(RecordLine.flags);

      memcpy(&RecordLine.price.mantissa,cursor,sizeof(RecordLine.price.mantissa));
      cursor+=sizeof(RecordLine.price.mantissa);

      if(    getBit(0,RecordLine.flags)
	 && !getBit(1,RecordLine.flags)
	 && !getBit(2,RecordLine.flags))
	{
	  RecordLine.side='A';
	}
      else if(!getBit(0,RecordLine.flags)
	 &&    getBit(1,RecordLine.flags)
	 &&   !getBit(2,RecordLine.flags))
	{
	  RecordLine.side='a';
	}
      else if(getBit(0,RecordLine.flags)
	 &&   getBit(1,RecordLine.flags)
	 &&  !getBit(2,RecordLine.flags))
	{
	  RecordLine.side='B';
	}
      else if(!getBit(0,RecordLine.flags)
	 &&   !getBit(1,RecordLine.flags)
	 &&    getBit(2,RecordLine.flags))
	{
	  RecordLine.side='b';
	}
      else if(getBit(0,RecordLine.flags)
	 &&  !getBit(1,RecordLine.flags)
	 &&   getBit(2,RecordLine.flags))
	{
	  RecordLine.side='T';
	}


      // large or small price?
      if(getBit(7,RecordLine.flags))
	{
	  read=fread(&input_data,2,1,inputfh);
	  
	  memcpy(&PriceSmall,
		 input_data,
		 sizeof(PriceSmall));
	  
	  RecordLine.price.integer=PriceSmall;
	}
      else
	{
	  read=fread(&input_data,4,1,inputfh);
	  memcpy(&RecordLine.price.integer,
		 input_data,
		 sizeof(RecordLine.price.integer));
	}

      // convert the price into a string
      char* buf=parse_price_out(RecordLine.price,buf);

      // small or large size
      if(getBit(6,RecordLine.flags))
	{	  
	  read=fread(&input_data,2,1,inputfh);
	  
	  memcpy(&SizeSmall,
		 input_data,
		 sizeof(SizeSmall));
	  
	  RecordLine.size=SizeSmall;
	}
      else
	{
	  read=fread(&input_data,4,1,inputfh);
	  
	  memcpy(&RecordLine.size,
		 input_data,
		 sizeof(RecordLine.size));
	}

      // new or previous exchange
      if(getBit(5,RecordLine.flags))
	{
	  RecordLine.exchange=last_exchange;
	}
      else
	{
	  read=fread(&input_data,1,1,inputfh);
	  
	  memcpy(&RecordLine.exchange,
		 input_data,
		 sizeof(RecordLine.exchange));
	}
	                
      // diff or full timestamp?
      if(getBit(4,RecordLine.flags))
	{	  
	  read=fread(&input_data,1,1,inputfh);
	  
	  memcpy(&RecordLine.sendtimediff,
		 input_data,
		 sizeof(RecordLine.sendtimediff));
	  
	  RecordLine.sendtime=last_time+RecordLine.sendtimediff;
	}
      else
	{
	  read=fread(&input_data,4,1,inputfh);
	  
	  memcpy(&RecordLine.sendtime,
		 input_data,
		 sizeof(RecordLine.sendtime));
	}

      // same times or different?
      if(getBit(3,RecordLine.flags))
	{
	  RecordLine.recvtime=RecordLine.sendtime;
	}
      else
	{
	  read=fread(&input_data,4,1,inputfh);
	  
	  memcpy(&RecordLine.recvtime,
		 input_data,
		 sizeof(RecordLine.recvtime));
	}
      

      // look up the ID
      symbol=FindDictListSymbol(*entry,ticker_dictionary_start);
      
      fprintf(outputfh, "%s,%c,%c,%c,%d,%d,%s,%d%s",
	      symbol,
	      RecordLine.exchange,
	      RecordLine.side,
	      RecordLine.condition,
	      RecordLine.sendtime,
	      RecordLine.recvtime,
	      buf,
	      RecordLine.size,
	      LINEEND
	      );
      
      free(buf);
      
      last_time=RecordLine.sendtime;
      last_exchange=RecordLine.exchange;
    }
}
  
int main (int argc, char **argv)
{
  /* default compressing */
  bool  compress = true;

  /* filehandles */
  char *InputFileName = NULL;
  char *OutputFileName = NULL;
  FILE *inputfh, *outputfh;

  /* list to keep the dictionary */
  ticker_dict_t* ticker_dictionary_start=NULL;
  
  int options;


  opterr = 0;

  while ((options = getopt (argc, argv, "cdx")) != -1)
    switch (options)
      {
      case 'c':
        compress = 1;
        break;
      case 'd':
        compress = 0;
        break;
      case 'x':
	debug =1;
	break;
      case '?':
        if (isprint (optopt))
          fprintf (stderr, "Unknown option `-%c'.\n", optopt);
        else
          fprintf (stderr,
                   "Unknown option character `\\x%x'.\n",
                   optopt);
        return 1;
      default:
        abort ();
      }


  if ( argc - optind <= 1 || argc - optind > 2 )
    {
      usage();
      exit(-1);
    }
  
  InputFileName=malloc(strlen(argv[optind]));
  strcpy(InputFileName,argv[optind]);
  inputfh = fopen(InputFileName,"r");
  if(inputfh == NULL)
    {
      perror("Error opening inout file");
      exit(-1);
    }
    
  OutputFileName=malloc(strlen(argv[optind+1]));
  strcpy(OutputFileName,argv[optind+1]);  
  outputfh = fopen(OutputFileName,"w+");   /* we overwrite the file if it exists -
					      TODO:should be tested first... */
  if(outputfh == NULL)
    {
      perror("Error opening output file");
      exit(-1);
    }
    
  if (compress)
    {
      printf("compressing...%s into %s\n",InputFileName,OutputFileName);
      do_compress(inputfh,outputfh,ticker_dictionary_start);
    }
  else
    {
      printf("decompressing...%s into %s\n",InputFileName,OutputFileName);
      do_decompress(inputfh,outputfh,ticker_dictionary_start);
    }


  DestroyDictList(ticker_dictionary_start);

  fclose(inputfh);
  fclose(outputfh);

  free(InputFileName);
  free(OutputFileName);

  return 0;
}

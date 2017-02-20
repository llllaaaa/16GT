#include <execinfo.h>
#include <signal.h>

#include "dependencies.h"
#include "CPUfunctions.h"
#include "VariantCaller.h"
#include "FisherExactTest.h"
#include "SnapshotHandler.h"

static void sigsegvHandler(int sig, siginfo_t *si) {
  if ((si->si_code & SEGV_MAPERR) || si->si_code == 128) {
    void *array[50];
    char **messages;
    int size, i;

    fprintf(stderr, "signal %d (%s), address is %p\n", sig, strsignal(sig), si->si_addr);
    size = backtrace(array, 50);
    messages = backtrace_symbols(array, size);

    for (i = 1; i < size && messages != NULL; ++i) {
      fprintf(stderr, "[bt]: (%d) %s\n", i, messages[i]);
    }
    free(messages);
    _exit(EXIT_FAILURE);
  } else {
    fprintf(stderr, "Should never reach here, si_code: %d.\n", si->si_code);
    _exit(EXIT_FAILURE);
  }
}

struct sigaction sigsegvSA;

void printCommandUsage(char *programName) {
  fprintf(stderr, "Usage:\n");
  fprintf(stderr, "%s -i <Reference Index Prefix> -o <Output Prefix> [-e regionListFile]\n", programName);
  fprintf(stderr, "    -o: Output Prefix\n");
  fprintf(stderr, "    -e <Exome Region Index>: Exome Region Index generated by RegionIndexBuilder\n");
  fprintf(stderr, "    -v: Be verbose\n");
}

void parseCommandlineArguments(int argn, char *args[], char *&indexPrefix, InputOptions &input_options) {
  indexPrefix = NULL;
  input_options.resultPrefix = NULL;
  input_options.isExome = false;
  input_options.exomeRegionFileName = NULL;
  input_options.resultPrefix = NULL;
  input_options.verbose = false;

  if (argn <= 1) {
    printCommandUsage(args[0]);
    exit(1);
  }

  for (int i = 1; i < argn; ++i) {

    char *flag = args[i];
    if (strcmp(flag, "-i") == 0) {
      if (indexPrefix) {
        fprintf(stderr, "Usage Error: Multiple indexes specified.\n\n");
        printCommandUsage(args[0]);
        exit(1);
      }
      indexPrefix = args[++i];
    } else if (strcmp(flag, "-o") == 0) {
      if (input_options.resultPrefix) {
        fprintf(stderr, "Usage Error: Multiple Output Prefixes specified.\n");
        printCommandUsage(args[0]);
        exit(1);
      }
      input_options.resultPrefix = args[++i];
    } else if (strcmp(flag, "-e") == 0) {
      if (input_options.exomeRegionFileName) {
        fprintf(stderr, "Usage Error: Multiple Exome Region Index specified.\n");
        printCommandUsage(args[0]);
        exit(1);
      }
      input_options.exomeRegionFileName = args[++i];
      input_options.isExome = true;
    } else if (strcmp(flag, "-v") == 0) {
      input_options.verbose = true;
    } else {
      fprintf(stderr, "Usage Error: Invalid arguments.\n");
      printCommandUsage(args[0]);
      exit(1);
    }
  }

  if (!indexPrefix) {
    fprintf(stderr, "Usage Error: Missing index.\n");
    printCommandUsage(args[0]);
    exit(1);
  }

  if (!input_options.resultPrefix) {
    fprintf(stderr, "Usage Error: Missing Output Prefix.\n");
    printCommandUsage(args[0]);
    exit(1);
  }

  if (!dirOfPrefixExists(indexPrefix)) {
    fprintf(stderr, "Error: The directory of Reference Index doesn't exists.\n");
    exit(1);
  }

  if (!dirOfPrefixExists(input_options.resultPrefix)) {
    fprintf(stderr, "Error: The directory of Output Prefix doesn't exists.\n");
    exit(1);
  }
}


int main(int argc, char **argv) {
  
  sigsegvSA.sa_flags = SA_SIGINFO;
  sigemptyset(&sigsegvSA.sa_mask);
  sigsegvSA.sa_sigaction = sigsegvHandler;
  if (sigaction(SIGSEGV, &sigsegvSA, NULL) == -1) {
    fprintf(stderr, "sigsegv singal handler registration error\n");
    exit(EXIT_FAILURE);
  }

  BWT *bwt;
  BWT *revBwt;
  HSP *hsp;
  uint numOfOccValue;
  uint *lkt, *revLkt;
  uint *revOccValue, *occValue;
  uint lookupWordSize;
  double startTime, indexLoadTime;
  double lastEventTime;

  InputOptions input_options;
  char *iniFileName;
  iniFileName = (char *) xmalloc(strlen(argv[0]) + 5);
  strcpy(iniFileName, argv[0]);
  strcpy(iniFileName + strlen(argv[0]), ".ini");
  IniParams ini_params;

  if (ParseIniFile(iniFileName, ini_params) != 0) {
    fprintf(stderr, "Failed to parse configuration file ... %s\n", iniFileName);
    return 1;
  }
  xfree(iniFileName);

  char *indexPrefix;
  parseCommandlineArguments(argc, argv, indexPrefix, input_options);

  IndexFileNames index;
  processIndexFileName(index, indexPrefix, ini_params);

  printf("Loading reference index...\n");
  
  startTime = setStartTime();
  loadIndex(index, &bwt, &revBwt, &hsp, &lkt, &revLkt,
            &revOccValue, &occValue, numOfOccValue, lookupWordSize, ini_params.Ini_shareIndex);
  indexLoadTime = getElapsedTime(startTime);
  printf("Done in %9.4f seconds\n", indexLoadTime);
  printf("Reference sequence length: %u\n\n", bwt->textLength);
  lastEventTime = indexLoadTime;

  SnpCounter *snpCounter;
  SnpOverflowCounterArray *snpOverflowCounterArray;
  unsigned int *invalidSnpCounterPos;
  MemoryPool *pool;

  hsp->snpBundle.numOfCPUThreads = ini_params.Ini_NumOfCpuThreads;
  hsp->snpFlag = SNP_STAT_FLAG;

  ExomeRegion *exomeRegion = NULL;
  unsigned int numExomeRegion = 0;
  if (hsp->snpFlag & SNP_STAT_FLAG) {
    FILE *exomeFile = NULL;
    if (input_options.isExome) {
      exomeFile = fopen(input_options.exomeRegionFileName, "rb");
      if (!exomeFile) {
        fprintf(stderr, "Exome Region Index %s doesn't exist\n", input_options.exomeRegionFileName);
        exit(1);
      } else {
        fread(&numExomeRegion, sizeof(unsigned int), 1, exomeFile);
        exomeRegion = (ExomeRegion *) malloc(numExomeRegion * sizeof(ExomeRegion));
        fread(exomeRegion, sizeof(ExomeRegion), numExomeRegion, exomeFile);
        if (!exomeRegion) {
          fprintf(stderr, "Error loading Exome Region Index\n");
          exit(1);
        }
      }
      fclose(exomeFile);

      printf("%u Exome Regions loaded\n", numExomeRegion);
      double exomeRegionLoadTime = getElapsedTime(startTime);
      printf("Elapsed time : %9.4f seconds\n\n", exomeRegionLoadTime - lastEventTime);
      lastEventTime = exomeRegionLoadTime;
    }
  }

  BWTFree(bwt, ini_params.Ini_shareIndex);
  BWTFree(revBwt, ini_params.Ini_shareIndex);

  if (hsp->snpFlag & SNP_STAT_FLAG) {
    printf("Reading snapshot...\n");

    char *snapshotFileName;
    snapshotFileName = (char *) malloc(strlen(input_options.resultPrefix) + 10);

    sprintf(snapshotFileName, "%s.snapshot", input_options.resultPrefix);

    readSnpInfoSnapshot(&(hsp->snpBundle), hsp->dnaLength, snapshotFileName);

    free(snapshotFileName);

    snpCounter = hsp->snpBundle.snpCounter;
    snpOverflowCounterArray = hsp->snpBundle.snpOverflowCounterArray;
    pool = hsp->snpBundle.snpMemoryPool;
    invalidSnpCounterPos = hsp->snpBundle.invalidSnpCounterPos;

    double snapshotLoadTime = getElapsedTime(startTime);
    printf("Read snapshot in %9.4f seconds\n\n", snapshotLoadTime - lastEventTime);
    lastEventTime = snapshotLoadTime;

    printf("Handling SNP Counter Result\n");

    
    char *snp_noRF_filename = (char *) malloc(strlen(input_options.resultPrefix) + 12);
    char *snp_filename = (char *) malloc(strlen(input_options.resultPrefix) + 10);

    sprintf(snp_noRF_filename, "%s.tmpresult", input_options.resultPrefix);
    sprintf(snp_filename, "%s.txt", input_options.resultPrefix);

    LikelihoodCache *likelihood_cache = (LikelihoodCache *) malloc(sizeof(LikelihoodCache));
    prefill_likelihood_cache_with_p_err(likelihood_cache, ini_params.Ini_BalanceSubError,
                                        ini_params.Ini_UnbalanceSubError);

    unsigned int attriSize = 0;
    selectPossibleSNPs(hsp->snpBundle, hsp->packedDNA, hsp->dnaLength, hsp->annotation, &attriSize,
                       input_options.isExome, exomeRegion, numExomeRegion, likelihood_cache, input_options,
                       ini_params, hsp->ambiguityMap, hsp->translate, snp_noRF_filename,
                       startTime, lastEventTime);

    double handleSnpResultTime = getElapsedTime(startTime);
    printf("Elapsed time : %9.4f seconds\n\n", handleSnpResultTime - lastEventTime);
    lastEventTime = handleSnpResultTime;

    printf("Writing variants to %s\n", snp_filename);
    calRFpedictProb(snp_noRF_filename, snp_filename, attriSize, input_options.verbose);
    double rfFillTime = getElapsedTime(startTime);
    printf("Elapsed time : %9.4f seconds\n\n", rfFillTime - lastEventTime);
    lastEventTime = rfFillTime;

    remove(snp_noRF_filename);

    free(snp_noRF_filename);
    free(snp_filename);

    destroySnpOverflowCounter(snpOverflowCounterArray, hsp->snpBundle.numOfCPUThreads);
    destroyMemoryPool(pool);
    destroySnpCounter(snpCounter, hsp->dnaLength);
    xfree(invalidSnpCounterPos);

    free(likelihood_cache);
  }

  HSPFree(hsp, 1, ini_params.Ini_shareIndex);

  printf("Total Running Time: %9.4f\n\n", getElapsedTime(startTime));
  return 0;
}

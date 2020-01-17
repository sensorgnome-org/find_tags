/*

  find_tags_unifile: Find sequences of pulses corresponding to bursts from
  tags registered in a database, but where frequency settings and pulses
  from all antennas are present in a single file.

  Optionally pre-filter pulse stream with a rate-limiting buffer.

  This version is greedy: a tag ID is emitted as soon as a full burst
  is recognized, and participating pulses are removed from
  consideration in other bursts.  An effort is made to identify runs
  of consecutive bursts from each tag, but well-timed noise can cause
  these to be split, even if all bursts are detected.

  This version accepts a continuous sequence of pulses from an
  antenna, together with an auxiliary file of time-stamped antenna
  frequency settings.  This avoids having to split raw pulse data into
  separate files by antenna frequency.  Also, we maintain a list of
  all tags at known frequencies, requiring only that IDs be unique
  within frequency.  When returning a tag hit, we supply tag ID and
  frequency.  Hits are only accepted when the range of frequency
  estimates of component pulses is within a command-line specified
  tolerance.

  We could simplify the code somewhat by this scheme:

  - merge tag databases across frequencies
  - assign a frequency to each pulse equal to antenna frequency + pulse
    frequency offset
  - run the usual algorithm, without worrying about antenna frquency
  - the frequency slop filter would ensure we didn't assemble tag bursts
    from pulses at different nominal frequencies.

  However, merging the tag ID databases across frequencies might not
  be desirable, especially if the sets of IDs at different frequencies
  have a large symmetric difference, as then our false positive
  rates (overall, not per tag ID) will go up.

  New simple greedy tag-extractor:

  First candidate accepting K consecutive pulses from a tag ID wins,
  and any other candidate locked on the same tag ID in the same
  interval is killed off.

  Pulses are only accepted if the frequency difference isn't too big.

  A Tag_Candidate is in one of three quality levels:
  - MULTIPLE: more than one tag ID is compatible with pulses accepted so far
  - SINGLE: only one tag ID is compatible with pulses accepted so far
  - CONFIRMED: only one tag ID is compatible, and we've seen at least K consecutive pulses

  As soon as a candidate is confirmed, it kills off any TCs with
  the same ID or sharing any pulses.

  Copyright 2012-2018 John Brzustowski

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA

- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

   A tag emits a sequence of pulses of fixed length at a fixed VHF
   frequency.  The pulses are organized as bursts of PULSES_PER_BURST
   pulses.  For a tag with given ID, the spacing ("gaps") between
   pulses in a burst is fixed, and differs from that of tags with all
   other IDs.  The spacing ("burst interval") between consecutive
   bursts is also fixed for a tag, but can be the same as that of tags
   with other IDs.

   The (full-burst) tag recognition problem can be coded as an NDFA:

     Nodes:

       - a pair (Tag_ID_Set, phase).  This is the set of tag IDs
       compatible with the edges on the path from the root to this
       node.  Phase is the depth of the node in the tree (i.e. the
       number of edges traversed from root to this node).

       - the unique root has Tag_ID_Set = {all registered tag IDs} and
       phase = 0.

     Edges:

       - There is a directed edge labelled G from node (T1, p1) to node (T2, p2)
         iff:
               p2 = p1 + 1 (except for back edges and loops - see below)
           and T2 is a subset of T1
           and G is a valid gap at phase p1 for all tags in T2

	- edges represent recognition of a valid (for some subset of
          tags) gap between pulses

        - to allow for measurement error, there are multiple edges between any
	  two adjacent nodes, one for each G in some range [g - slop, g + slop],
	  down to some finite precision.

     Back Edges:

        -  to allow identification of consecutive hits from the same tag,
           the NDFA is augmented with back edges from the last pulse of
           two consecutive bursts to the start of the second burst.  Because
           tags are uniquely identified in a single burst, this allows the
           NDFA to keep recognizing the same tag when it sees bursts
           of the correct gap signature and interval spacing.

     Loops:

        -  to allow the NDFA to ignore a noise pulse, for each node N and each
           edge labelled G from N to a different node M != N, a loop edge
           labelled G is added to N.  This edge corresponds to ignoring a noise
           pulse.

   Directed paths through the NDFA graph starting at root correspond
   to sequences of pulses generated by a single tag, starting at phase
   0, with no pulses missing, and with 0 or more intervening noise
   pulses.  Paths ending at a node in phase PULSES_PER_BURST - 1 have
   a unique edge-label sequence, since this is how tag IDs are encoded
   (i.e. the gaps between PULSES_PER_BURST pulses determine the tag).

   We use the usual subset construction to get a DFA corresponding to
   the NDFA without loop edges.  For loop edges, it seemed easier to
   clone the DFA whenever a pulse is added, with one copy accepting the
   pulse and the other not.

   A separate NDFA graph is built for each nominal VHF frequency in the
   tag database.

   Pulses are processed one at a time.  The current antenna frequency
   is used to selects which set of current tag candidates
   (i.e. running DFAs) is examined.  The pulse is added to each tag
   candidate with which it is compatible (given the time gap to the
   candidate's previous pulse, and the offset frequencies of candidate
   and pulse).  Usually, a DFA to which the pulse is added is also cloned
   before adding the pulse, i.e. the DFA is forked to allow for
   non-addition of the pulse, in case it is noise.  The exception is that
   we don't clone DFAs which already point to a single tag ID unless
   the new pulse is the first of a burst.  The reason to clone in this case
   is that we want to allow for large burst slop without derailing recognition
   of series of consecutive bursts from the same tag.  If we didn't clone,
   accepting a noise pulse within the large burst-slop window would soon kill
   the tag candidate (i.e. when no further pulses from the tag were seen).

   Tag_Candidate ages are compared to the time of the new pulse, and
   any which are too old are destroyed.  If a Tag_Candidate enters the
   CONFIRMED tag ID level, all bursts seen so far are output, and any
   other Tag_Candidates sharing any pulses or having the same unique
   tag ID are destroyed, so that every pulse is used for at most one
   recognized burst.  If a pulse does not cause any existing DFA to
   reach the CONFIRMED level, then a new DFA is started at that pulse.

   False Positive Rate

   Phil Taylor came up with this idea of how to estimate false positive
   rate:  keep track of the number of discarded detections of each tag ID,
   and report it at each dump of a confirmed hit.  Reset the count then.
   So actually, with each confirmed hit, we report the number of discarded
   hits per second since the last confirmed hit.

*/

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <getopt.h>
#include <cmath>
#include <limits>
#include <list>
#include <map>
#include <set>
#include <vector>
#include <string.h>

#include "find_tags_common.hpp"
#include "Freq_Setting.hpp"
//#include "Freq_History.hpp"
#include "Tag_Database.hpp"
#include "Pulse.hpp"
#include "Tag_Candidate.hpp"
#include "Tag_Finder.hpp"
#include "Rate_Limiting_Tag_Finder.hpp"
#include "Tag_Foray.hpp"

//#define FIND_TAGS_DEBUG

void
usage() {
  puts (
	"Usage:\n"
	"    find_tags [OPTIONS] TAGFILE.[csv|sqlite] [UNIFIED.CSV]\n"
	"where:\n\n"

	"TAGFILE.csv or TAGFILE.sqlite  is a file holding a table of registered tags\n"
	"    where TAGFILE.csv is a Comma-Separated Values file with this header:\n"
        "        proj,id,tagFreq,fcdFreq,g1,g2,g3,bi,dfreq,g1.sd,g2.sd,g3.sd,bi.sd,dfreq.sd,filename\n"
        "    or TAGFILE.sqlite has (at least) these columns:\n"
        "        proj, id, tagFreq, fcdFreq, dfreq, g1, g2, g3, bi\n"
	"    with:\n"
	"        proj:     quoted string identifying project these tags belong to\n"
	"        id:       integer giving manufacturer tag ID\n"
	"        tagFreq:  (MHz) nominal tag frequency\n"
	"        fcdFreq:  (MHz) tuner frequency at which tag was registered\n"
	"        gN:       (ms) time (gap) between pulses (N, N+1) in burst\n"
	"        bi:       (s) time between consecutive bursts\n"
	"        dfreq:    (kHz) apparent offset of tag from tuner frequency\n"
	"        gN.sd:    (ms) standard deviation of time between pulses (N, N+1)\n"
	"        bi.sd:    (s) standard deviation of time between bursts\n"
	"        dfreq.sd: (kHz) standard deviation of offset frequency\n"
	"        filename: quoted string giving name of raw .WAV file (if any) recorded\n"
	"            at tag registration\n\n"

	"UNIFIED.CSV is a file holding raw sensorgnome output records, including parameter\n"
        "    settings and GPS fixes.\n"
	"    Relvant lines are pANT,TS,DFREQ,SIG,NOISE\n"
	"    with:\n"
        "        ANT:    port code 'pX'\n"
	"        TS:     real timestamp (seconds since 1 Jan, 1970 00:00:00 GMT)\n"
	"        DFREQ:  (kHz) estimated pulse offset frequency\n"
	"        SIG:    (dB) estimated relative pulse signal strength\n"
	"        NOISE:  (dB) estimated relative noise level near pulse\n"
        "    or S,TS,PORT,-m,FREQ,RC,ERR\n"
        "    with:\n"
        "        TS:   real timestamp\n"
        "        PORT: port number\n"
        "        -m:   indicates frequency setting in megahertz\n"
        "        FREQ: frequency in MHz to which port is being set\n"
        "        RC:   zero if frequency setting succeeded, else non-zero error code\n"
        "        ERR:  blank on success, else error message\n"

	"    If UNIFIED.CSV is unspecified, unified data are read from stdin\n\n"

	"and OPTIONS can be any of:\n\n"

	"-f, --default-freq=FREQ\n"
	"    the default antenna frequency, in MHz\n"
        "    Although input data files contain frequency setting records, this option can\n"
        "    be used to set the frequency when processing a fragment of an input file that\n"
        "    doesn't contain an initial frequency setting\n\n"

        "-F --force-default-freq\n"
        "    Ignore frequency settings in the input dataset; always assume\n"
        "    all receivers are tuned to the default frequency\n\n"

	"-b, --burst-slop=BSLOP\n"
	"    how much to allow time between consecutive bursts\n"
	"    to differ from measured tag values, in millseconds.\n"
	"    default: 20 ms\n\n"

	"-B, --burst-slop-expansion=BSLOPEXP\n"
	"    how much to increase burst slop for each missed burst\n"
	"    in milliseconds; meant to allow for clock drift\n"
	"    default: 1 ms / burst\n\n"

	"-c, --pulses-to-confirm=CONFIRM\n"
	"    how many pulses must be detected before a hit is confirmed.\n"
	"    By default, CONFIRM = PULSES_PER_BURST, but for more stringent\n"
	"    filtering when BSLOP is large, CONFIRM should be set to PULSES_PER_BURST + 2\n"
	"    or larger, so that more gaps must match those registered for a given tag\n"
	"    before a hit is reported.\n"
	"    default: PULSES_PER_BURST (i.e. 4)\n\n"

	"-H, --header-only\n"
	"    output the header ONLY; does no processing.\n\n"

	"-l, --signal-slop=SSLOP\n"
	"    tag signal strength slop, in dB.  A tag burst will only be recognized\n"
	"    if its dynamic range (i.e. range of signal strengths of its component pulses)\n"
	"    is within SSLOP.  This limit applies within each burst of a sequence\n"
	"    default: 10 dB\n\n"

	"-m, --min-dfreq=MINDFREQ\n"
	"    minimum offset frequency, in kHz.  Pulses with smaller offset frequency\n"
	"    are dropped.\n"
	"    default: -Inf (i.e. no minimum)\n\n"

	"-M, --max-dfreq=MAXDFREQ\n"
	"    maximum offset frequency, in kHz.  Pulses with larger offset frequency\n"
	"    are dropped.\n"
	"    default: Inf (i.e. no minimum)\n\n"

	"-n, --no-header\n"
	"    don't output the column names header; useful when output\n"
	"    is to be appended to an existing .CSV file.\n\n"

	"-p, --pulse-slop=PSLOP\n"
	"    how much to allow time between consecutive pulses\n"
	"    in a burst to differ from measured tag values, in milliseconds\n"
	"    default: 1.5 ms\n\n"

	"-R, --max-pulse-rate=MAXPULSERATE\n"
	"    maximum pulse rate (pulses per second) during pulse rate time window;\n"
	"    Used to prevent exorbitant memory usage and execution time when noise-\n"
	"    or bug-induced pulse bursts are present.  Pulses from periods of length\n"
	"    PULSERATEWIN (specified by --pulse-rate-window) where \n"
	"    the pulse rate exceeds MAXPULSERATE are simply discarded.\n\n"
	"    default: 0 pulses per second, meaning no rate limiting is done.\n\n"

	"-s, --frequency-slop=FSLOP\n"
	"    tag frequency slop, in KHz.  A tag burst will only be recognized\n"
	"    if its bandwidth (i.e. range of frequencies of its component pulses)\n"
	"    is within FSLOP.  This limit applies to all bursts within a sequence\n"
	"    default: 2 kHz\n\n"

	"-S, --max-skipped-bursts=SKIPS\n"
	"    maximum number of consecutive bursts that can be missing (skipped)\n"
	"    without terminating a run.  When using the pulses_to_confirm criterion\n"
	"    that number of pulses must occur with no gaps larger than SKIPS bursts\n"
	"    between them.\n"
	"    default: 60\n\n"

	"-t, --test\n"
	"    verify that the tag database is valid and that all tags in it can be\n"
	"    distinguished with the specified algorithm parameters.\n"
	"    If the database is valid and all tags are distinguishable, the program\n"
        "    prints 'Okay\\n' to stderr and the exist code is 0.  Otherwise, the exist\n"
        "    code is -1 and an error message is printed to stderr.\n\n"

	"-u, --unsigned-dfreq=[0|1]\n"
	"    if non-zero, the sign on frequency offsets of pulses is ignored\n"
	"    default: 0 (sign of frequency offset is used when assembling bursts\n\n"

	"-w, --pulse-rate-window=PULSERATEWIN\n"
	"    the time window (seconds) over which pulse-rate is measured.  When pulse\n"
	"    rate exceeds the value specified by --max-pulse-rate during a period of\n"
	"    PULSERATEWIN seconds, all pulses in that period are discarded.\n"
	"    default: 60 seconds (but this only takes effect if -R is specified)\n\n"

	);
}

int
main (int argc, char **argv) {
      enum {
	OPT_BURST_SLOP	         = 'b',
	OPT_BURST_SLOP_EXPANSION = 'B',
	OPT_PULSES_TO_CONFIRM    = 'c',
	OPT_DEFAULT_FREQ         = 'f',
	OPT_FORCE_DEFAULT_FREQ   = 'F',
        COMMAND_HELP	         = 'h',
	OPT_HEADER_ONLY	         = 'H',
	OPT_SIG_SLOP	         = 'l',
	OPT_MIN_DFREQ            = 'm',
	OPT_MAX_DFREQ            = 'M',
	OPT_NO_HEADER	         = 'n',
        OPT_PULSE_SLOP	         = 'p',
	OPT_MAX_PULSE_RATE       = 'R',
	OPT_FREQ_SLOP	         = 's',
	OPT_MAX_SKIPPED_BURSTS   = 'S',
        OPT_TEST                 = 't',
        OPT_UNSIGNED_DFREQ       = 'u',
	OPT_PULSE_RATE_WINDOW    = 'w'
    };

    int option_index;
    static const char short_options[] = "b:B:c:f:FhHl:m:M:np:R:s:S:tu:w:";
    static const struct option long_options[] = {
        {"burst-slop"		   , 1, 0, OPT_BURST_SLOP},
        {"burst-slop-expansion"    , 1, 0, OPT_BURST_SLOP_EXPANSION},
	{"pulses-to-confirm"	   , 1, 0, OPT_PULSES_TO_CONFIRM},
        {"default-freq"		   , 1, 0, OPT_DEFAULT_FREQ},
        {"force-default-freq"      , 0, 0, OPT_FORCE_DEFAULT_FREQ},
        {"help"			   , 0, 0, COMMAND_HELP},
	{"header-only"		   , 0, 0, OPT_HEADER_ONLY},
	{"signal-slop"             , 1, 0, OPT_SIG_SLOP},
	{"min-dfreq"               , 1, 0, OPT_MIN_DFREQ},
	{"max-dfreq"               , 1, 0, OPT_MAX_DFREQ},
	{"no-header"		   , 0, 0, OPT_NO_HEADER},
        {"pulse-slop"		   , 1, 0, OPT_PULSE_SLOP},
	{"max-pulse-rate"          , 1, 0, OPT_MAX_PULSE_RATE},
        {"frequency-slop"	   , 1, 0, OPT_FREQ_SLOP},
	{"max-skipped-bursts"      , 1, 0, OPT_MAX_SKIPPED_BURSTS},
	{"pulse-rate-window"       , 1, 0, OPT_PULSE_RATE_WINDOW},
        {"test"                    , 0, 0, OPT_TEST},
        {"unsigned-dfreq"          , 1, 0, OPT_UNSIGNED_DFREQ},
        {0, 0, 0, 0}
    };

    int c;

    Frequency_MHz default_freq = 0;

    string tag_filename;
    string pulse_filename = "";

    bool header_desired = true;

    float min_dfreq = -std::numeric_limits<float>::infinity();
    float max_dfreq = std::numeric_limits<float>::infinity();
    bool unsigned_dfreq = false;

    bool force_default_freq = false;
    bool test_only = false;
    // rate-limiting buffer parameters

    float max_pulse_rate = 0;    // no rate-limiting
    Gap pulse_rate_window = 60;  // 1 minute window
    Gap min_bogus_spacing = 600; // emit bogus tag ID at most once every 10 minutes

    while ((c = getopt_long(argc, argv, short_options, long_options, &option_index)) != -1) {
        switch (c) {
        case OPT_BURST_SLOP:
	  Tag_Finder::set_default_burst_slop_ms(atof(optarg));
	  break;
        case OPT_BURST_SLOP_EXPANSION:
	  Tag_Finder::set_default_burst_slop_expansion_ms(atof(optarg));
	  break;
	case OPT_PULSES_TO_CONFIRM:
	  Tag_Candidate::set_pulses_to_confirm_id(atoi(optarg));
	  break;
	case OPT_DEFAULT_FREQ:
	  default_freq = atof(optarg);
	  break;
	case OPT_FORCE_DEFAULT_FREQ:
          force_default_freq = true;
          break;
        case COMMAND_HELP:
            usage();
            exit(0);
	case OPT_HEADER_ONLY:
	  Tag_Candidate::output_header(&std::cout);
	  exit(0);
        case OPT_SIG_SLOP:
	  Tag_Candidate::set_sig_slop_dB(atof(optarg));
	  break;
	case OPT_MIN_DFREQ:
	  min_dfreq = atof(optarg);
	  break;
	case OPT_MAX_DFREQ:
	  max_dfreq = atof(optarg);
	  break;
	case OPT_NO_HEADER:
	  header_desired = false;
	  break;
        case OPT_PULSE_SLOP:
	  Tag_Finder::set_default_pulse_slop_ms(atof(optarg));
	  break;
	case OPT_MAX_PULSE_RATE:
	  max_pulse_rate = atof(optarg);
	  break;
        case OPT_FREQ_SLOP:
	  Tag_Candidate::set_freq_slop_kHz(atof(optarg));
	  break;
	case OPT_MAX_SKIPPED_BURSTS:
	  Tag_Finder::set_default_max_skipped_bursts(atoi(optarg));
	  break;
	case OPT_PULSE_RATE_WINDOW:
	  pulse_rate_window = atof(optarg);
	  break;
        case OPT_TEST:
          test_only = true;
          header_desired = false;
          break;
        case OPT_UNSIGNED_DFREQ:
          unsigned_dfreq = atoi(optarg);
          break;
        default:
            usage();
            exit(1);
        }
    }


    if (optind == argc) {
      usage();
      exit(1);
    }

    tag_filename = string(argv[optind++]);
    if (optind < argc) {
      pulse_filename = string(argv[optind++]);
    }

    // set options and parameters

    try {
      Tag_Database tag_db (tag_filename);

      // Freq_Setting needs to know the set of nominal frequencies
      Freq_Setting::set_nominal_freqs(tag_db.get_nominal_freqs());

      // open the input stream

      std::istream * pulses;
      if (pulse_filename.length() > 0) {
        pulses = new ifstream(pulse_filename.c_str());
      } else {
        pulses = & std::cin;
      }

      if (pulses->fail())
        throw std::runtime_error(string("Couldn't open input file ") + pulse_filename);

      if (header_desired)
        Tag_Candidate::output_header(&std::cout);

      Tag_Foray foray(tag_db, pulses, & std::cout, default_freq, force_default_freq, min_dfreq, max_dfreq, max_pulse_rate, pulse_rate_window, min_bogus_spacing, unsigned_dfreq);

      if (test_only) {
        foray.test(); // throws if there's a problem
        std::cerr << "Ok\n";
        exit(0);
      }

      foray.start();
    }
    catch (std::runtime_error& e) {
      std::cerr << e.what() << std::endl;
      exit(2);
    }
}

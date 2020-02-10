/*

  find_tags_motus: Find sequences of pulses corresponding to bursts from
  tags registered in a database, where frequency settings and pulses
  from all antennas are present in a single file.

  Differs from find_tags_unifile in that:

  - database identifies tags by motusTagID integer, and that's what
    is output for each detection.

  - output is appended directly to specified tables in a specified
    sqlite database.  See the file sgEnsureTables.R function in the motus
    package to for the database schema.

All output from a run of this program forms a new batch.
================================================================================

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

  Copyright 2012-2016 John Brzustowski

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
#include <boost/program_options.hpp>
#include <boost/any.hpp>
#include "find_tags_common.hpp"
#include "Freq_Setting.hpp"
#include "Tag_Database.hpp"
#include "Pulse.hpp"
#include "Tag_Candidate.hpp"
#include "Tag_Finder.hpp"
#include "Rate_Limiting_Tag_Finder.hpp"
#include "Tag_Foray.hpp"
#include "Data_Source.hpp"

#ifdef DEBUG
// force debugging methods to be emitted

void * tabfun[] = { (void *) &Tag_Database::getTagForMotusID };

#endif

namespace po = boost::program_options;

int
main (int argc, char **argv) {

  // frequency-related params

  Frequency_MHz default_freq;
  bool force_default_freq;
  float min_dfreq;
  float max_dfreq;
  bool unsigned_dfreq;
  Frequency_Offset_kHz frequency_slop;

  // timing-related params

  Gap pulse_slop;
  Gap burst_slop;
  Gap burst_slop_expansion;
  unsigned int timestamp_wonkiness;

  // input-related params

  std::string input_file;
  bool src_sqlite;
  bool lotek;
  std::string tag_database;
  bool use_events;
  int bootnum;
  bool resume;

  // output-related params

  std::string output_db;
  bool info_only;
  bool test_only;
  bool graph_only;
  bool pulses_only;
  double gps_min_dt;

  // rate-limiting buffer params

  float max_pulse_rate;
  Gap pulse_rate_window;
  Gap min_bogus_spacing;

  // other filtering params
  int pulses_to_confirm;
  float sig_slop_dB;
  int max_skipped_bursts;

  // additional params
  std::vector < std::string > external_param;

#ifdef ACTIVE_TAG_DIAGNOSTICS
  double active_tag_dump_interval = 0;
#endif // ACTIVE_TAG_DIAGNOSTICS

  po::options_description opt
    (
     "Find sequences of pulses corresponding to bursts from registered tags\n\n"
     "Usage:  find_tags_motus [OPTIONS] [TAG_DATABASE] [OUTPUT_DB] [INPUT_FILE]\n\n"
     "where OPTIONS are described below, and\n\n"
     "   TAG_DATABASE is equivalent to `--tag_database TAG_DATABASE`\n"
     "   OUTPUT_DB is equivalent to `--output_db OUTPUT_DB`\n"
     "   INPUT_FILE is equivalent to `--input_file INPUT_FILE`\n\n",
     120 // line width for help message
     );
  opt.add_options()
    ("help",
     "print help"
     )

    ("default_freq,f", po::value<Frequency_MHz>(& default_freq)->multitoken()->default_value(166.376),
     "the default antenna frequency, in MHz.  Although input data files contain "
     "frequency-setting records, this option can be used to set the frequency when "
     "processing a fragment of an input file that doesn't contain an initial frequency "
     "setting"
     )
    ("force_default_freq,F", po::value<bool>(& force_default_freq)->implicit_value(true)->default_value(false),
     "Ignore frequency settings in the input dataset; always assume all receivers "
     "are tuned to the default frequency"
     )
    ("min_dfreq,m", po::value<float>(&min_dfreq)->default_value(-std::numeric_limits<float>::infinity()),
     "minimum offset frequency, in kHz.  Pulses with smaller offset frequency are dropped."
     )
    ("max_dfreq,M", po::value<float>(&max_dfreq)->default_value(std::numeric_limits<float>::infinity()),
     "maximum offset frequency, in kHz.  Pulses with larger offset frequency are dropped."
     )
    ("unsigned_dfreq,u", po::value<bool>(& unsigned_dfreq)->implicit_value(true)->default_value(false),
     "ignore the sign of frequency offsets, as some versions of the pulse detection "
     "code do a poor job of estimating a negative frequency"
     )
    ("frequency_slop,s", po::value<Frequency_Offset_kHz>(&frequency_slop)->default_value(0.5),
     "tag frequency slop, in KHz.  A tag burst will only be recognized "
     "if its bandwidth (i.e. range of frequencies of its component pulses) "
     "is within FSLOP.  This limit applies across all bursts within a sequence"
     )

    ("pulse_slop,p", po::value<Gap>(&pulse_slop)->default_value(1.5),
     "how much to allow time between consecutive pulses in a burst to differ from measured "
     "tag values, in milliseconds"
     )
    ("burst_slop,b", po::value<Gap>(&burst_slop)->default_value(10),
     "how much to allow time between consecutive bursts to differ from measured tag values, in millseconds."
     )
    ("burst_slop_expansion,B", po::value<Gap>(&burst_slop_expansion)->default_value(1.0),
     "how much to increase burst slop for each missed burst in milliseconds; meant to allow for clock drift"
     )
    ("timestamp_wonkiness,T", po::value<unsigned int>(&timestamp_wonkiness)->default_value(0),
     "try to correct clock jumps in Lotek .DTA input data, where the clock sometimes "
     "jumps back and forth by an integer number of seconds.  "
     "MAX_JUMP is a small integer, giving the maximum amount by which the clock can jump "
     "and the maximum rounded number of cumulative jumped seconds in a run (the two "
     "use the same parameter value because the jumps are assumed to be unbiased.\n"
     "This option is only permitted if --lotek is specified.\n"
     "FIXME: only values of 0 or 1 are currently supported"
     )

    ("input_file", po::value< std::string >(&input_file)->default_value(""),
     "if `src_sqlite` is specified, this is a `.sqlite` database which contains "
     "table `files` (for sensorgnomes) or table `DTAtags` (for Lotek receivers).  "
     "Otherwise, it is a .csv file, defaulting to `stdin` if not specified.  "
     "Raw receiver records are read from the specified file."
     )
    ("src_sqlite,Q", po::value<bool>(& src_sqlite)->implicit_value(true)->default_value(false),
     "Treat `input_file` as an sqlite database and fetch paths to compressed data files "
     "from the `files` table if a sensorgnome, or from the `DTAtags` table if a Lotek "
     "receiver."
     )
    ("lotek,L", po::value<bool>(& lotek)->implicit_value(true)->default_value(false),
     "Indicates that input data come from a lotek receiver.  In this case, input lines "
     "have a different format: TS,ID,ANT,SIG,ANTFREQ,GAIN,CODESET with:\n"
     "   TS:     real timestamp (seconds since the unix epoch)\n"
     "   ID:     lotek tag ID\n"
     "   ANT:    single digit\n"
     "   SIG:    signal strength in Lotek receiver units\n"
     "   GAIN:   receiver gain setting, in Lotek receiver units\n"
     "   CODESET: 'Lotek3' or 'Lotek4'\n\n"
     "Each input record is used to generate a sequence of pulse records in SG format,"
     "and the program re-finds tags from these."
     )
    ("tag_database", po::value< std::string > (&tag_database),
     ".sqlite file which contains the `tags` (and possibly `events`) tables that "
     "define the tags to be sought (and possibly their activation history)"
     )
    ("use_events,e", po::value<bool>(& use_events)->implicit_value(true)->default_value(false),
     "Limit the search for specific tags to periods of time when they are known to be "
     "active.  These periods are specified by a table in the tag database called "
     "'events', which must have at least the columns 'ts', 'motusTagID', and 'event',"
     "where 'ts' (double) is the timestamp for event, in seconds since 1 Jan 1970 GMT;"
     "'tagID' (int) is the motus tagID; and 'event'(int) is 1 for activation, "
     "0 for deactivation.\n"
     "In almost all cases, you want this option, as it lets the tag finder search "
     "for a changing set of tags over the time period it processes.\n"
     "Note: If this option is *not* specified, then all tags in the database are sought"
     "and their detections reported over the entire timespan of the input file."
     )
    ("bootnum,n", po::value<int>(& bootnum)->default_value(1),
     "monotonic boot session for this run.  Each boot session (time between reboots) for a "
     "receiver is processed by a separate invocation of find_tags_motus.  The batch generated by "
     "the current invocation run will be associated with the specified bootnum "
     "in the output database."
     )
    ("resume,r", po::value<bool>(& resume)->implicit_value(true)->default_value(false),
     " Attempt to resume tag finding where it left off for this boot session.  This means:\n"
     "   - seek to the end of the previously-processed input timestamp, or to a new line "
     "     with the same timestamp as was saved\n"
     "   - any active tag runs and candidates are resumed"
     )

    ("output_db", po::value< std::string > (& output_db),
     ".sqlite file which contains tables `hits`, `runs`, `batches`, etc. where "
     "output from this program will be stored; this is typically a .motus "
     "receiver database"
     )
    ("info_only", po::value<bool>(& info_only)->implicit_value(true)->default_value(false),
     "only print program version and parameter info in JSON format, then exit"
     )
    ("test_only,t", po::value<bool>(& test_only)->implicit_value(true)->default_value(false),
     "verify that the tag database is valid and that all tags in it can be "
     "distinguished with the specified algorithm parameters. "
     "If the database is valid and all tags are distinguishable, the program "
     "prints 'Okay\\n' to stderr and the exit code is 0.  Otherwise, the exit "
     "code is -1 and an error message is printed to stderr."
     )
    ("graph_only,g", po::value<bool>(& graph_only)->implicit_value(true)->default_value(false),
     "Output a 'graphviz'-format file describing the transition graph for each nominal "
     "frequency.  These files will be called graph1.gv, graph2, gv, ...  You can "
     "visualize using the 'dot' program from http:graphviz.org like so:\n"
     "   dot -Tsvg graph1.gv > graph1.svg\n"
     "and then view graph1.svg in a web browser or inkscape https://inkscape.org\n"
     "If you specify this option, the program quits without processing any input data."
     )
    ("pulses_only,P", po::value<bool>(& pulses_only)->implicit_value(true)->default_value(false),
     "Only output a table called `pulses` with these columns:\n"
     "   - batchID batch number\n"
     "   - ts timestamp\n"
     "   - ant antenna number\n"
     "   - antFreq (MHz); antenna listen frequency\n"
     "   - dfreq (kHz); offset frequency of pulse\n"
     "   - sig relative pulse signal strength (dB max)\n"
     "   - noise relative noise strength (dB max)\n"
     "With this option, the program ignores the tag database (although it must still be "
     "specified) and only uses these options:\n"
     "--default_freq, --force_default_freq, --min_dfreq, --max_dfreq"
     )
    ("gps_min_dt,G", po::value<double>(&gps_min_dt)->default_value(3600),
     "Minimum time step, in seconds, between GPS fixes to be recorded from receiver "
     "data. A negative value means do not record any GPS timestamps to the output "
     "database.")

    ("max_pulse_rate,R", po::value<float>(&max_pulse_rate)->default_value(0),
     "maximum pulse rate (pulses per second) during pulse rate time window."
     "Used to prevent exorbitant memory usage and execution time when noise-"
     "or bug-induced pulse bursts are present.  Pulses from periods of length "
     "PULSERATEWIN (specified by --pulse_rate_window) where "
     "the pulse rate exceeds MAXPULSERATE are simply discarded.\n"
     "When `max_pulse_rate=0`, no rate limiting is performed"
     )
    ("pulse_rate_window,w", po::value<Gap>(&pulse_rate_window)->default_value(60),
     "time window (seconds) over which `pulse_rate` is measured.  When pulse_rate "
     "exceeds the value specified by `max_pulse_rate` during a period of "
     "this many seconds, all pulses in that period are discarded.\n"
     "This parameter is only used if `max_pulse_rate` is specified)"
     )
    ("min_bogus_spacing,X", po::value< Gap >(&min_bogus_spacing)->default_value(600),
     "minimum time in seconds between emitting a bogus tag ID that indicates suppression "
     "of noisy input\n"
     "FIXME:  no bogus tag ID is currently emitted when suppressing noisy input.  "
     "See Tag_Candidate::dump_bogus_burst()"
     )

    ("pulses_to_confirm,c", po::value<int>(&pulses_to_confirm)->default_value(PULSES_PER_BURST),
     "how many pulses must be detected before a hit is confirmed.  For more"
     "stringent filtering when BSLOP is large, CONFIRM should be set to 2 *"
     "PULSES_PER_BURST or larger, so that more gaps must match those"
     "registered for a given tag before a hit is output."
     )
    ("sig_slop,l", po::value<float>(&sig_slop_dB)->default_value(10), "tag signal strength slop, "
     "in dB.  A tag burst will only be recognized if its dynamic range (i.e. range of "
     "signal strengths of its component pulses is within SSLOP.  This limit applies "
     "within each burst of a sequence.\nNote: if SSLOP < 0, this means to ignore signal "
     "strength, which is appropriate for sources where signal strength is not in DB "
     "units, e.g. Lotek .DTA files"
     )
    ("max_skipped_bursts,S", po::value<int>(&max_skipped_bursts)->default_value(60),
     "maximum number of consecutive bursts that can be missing (skipped) "
     "without terminating a run.  When using the pulses_to_confirm criterion"
     "that number of pulses must occur with no gaps larger than `max_skipped_bursts` "
     "bursts between them."
     )

    // additional params

    ("external_param,x", po::value< std::vector< std::string > >(&external_param),
     "record an external parameter for this batch. This parameter's value should "
     "look like NAME=VALUE, where VALUE is a number.  This option can be used "
     "multiple times.\n"
     "Note:  the commit hash from the tag database's `meta` table is automatically "
     "recorded as external parameter `metadata_hash` to avoid a race condition, "
     "so this option should *not* be used for that purpose."
     )
#ifdef ACTIVE_TAG_DIAGNOSTICS
    ("active_tag_dump_interval,a", po::value<double>(&active_tag_dump_interval)->default_value(0.0),
     "how often, in seconds, to dump a list of active tagIDs for each input channel. "
     "Each dump is a line of the form TS,PORT,NOMFREQ,motusID1,motusID2,...motusIDN\n"
     "e.g. -a 3600 dumps the list of active tags once per hour\n"
     "Only values > 0 cause active tag lists to be dumped, so the default (0) does not."
     )
#endif // ACTIVE_TAG_DIAGNOSTICS
    ;

  po::positional_options_description popt;
  popt.add("tag_database", 1)
    .add("output_db", 1)
    .add("input_file", 1);

  po::variables_map vm;
  po::store(po::command_line_parser(argc, argv).
            options(opt).positional(popt).run(), vm);
  po::notify(vm);

  std::map<std::string, std::string> external_param_map; // to store any external parameters for recording

  for (auto ep = external_param.begin(); ep != external_param.end(); ++ep) {
    const char *arg = ep->c_str();
    const char *delim = strchr(arg, '=');
    if (delim) {
      external_param_map[std::string(arg, delim - arg)] = std::string(1 + delim);
    } else {
      throw std::runtime_error("the -x (--external_param) options requires an argument that looks like NAME=VALUE");
    };
  }
  Tag_Foray::set_default_pulse_slop_ms(pulse_slop);
  Tag_Foray::set_default_burst_slop_ms(burst_slop);
  Tag_Foray::set_default_burst_slop_expansion_ms(burst_slop_expansion);
  Tag_Foray::set_default_max_skipped_bursts(max_skipped_bursts);
  Tag_Foray::set_timestamp_wonkiness(timestamp_wonkiness);
#ifdef ACTIVE_TAG_DIAGNOSTICS
  Tag_Foray::set_active_tag_dump_interval(active_tag_dump_interval);
#endif // ACTIVE_TAG_DIAGNOSTICS
  Tag_Candidate::set_pulses_to_confirm_id(pulses_to_confirm);
  Tag_Candidate::set_sig_slop_dB(sig_slop_dB);
  Tag_Candidate::set_freq_slop_kHz(frequency_slop);

  // sanity checks

  if (resume && lotek) {
    throw std::runtime_error("Can't use --resume with a Lotek receiver");
  };
  if (timestamp_wonkiness > 0 && ! lotek) {
    throw std::runtime_error("must specify --lotek in order to use --timestamp_wonkiness=N with N > 0");
  }

  // set options and parameters

  string program_name("find_tags_motus");
  string program_version(PROGRAM_VERSION); // defined in Makefile
  double program_build_ts = PROGRAM_BUILD_TS; // defined in Makefile

  // doing help?
  if (vm.count("help")) {
    std::cout << opt << std::endl;
    return 0;
  }

  // doing info?
  if (info_only) {
    std::cout << "{\"version\":\"" << program_version << "\",\"build_ts\":" << std::setprecision(14) << program_build_ts << ",\"options\":[";
    int comma = 0;
    for (auto i = opt.options().begin(); i != opt.options().end(); ++i) {
      boost::any x;
      if ((*i)->semantic()->apply_default(x)) {
        if (comma++)
          std::cout << ",";
        std::cout << "{\"name\":\"" << (*i)->long_name() << "\",\"description\":\"" << (*i)->description() << "\",\"default\":";
        try {
          double y = boost::any_cast<double>(x);
          // JSON lacks support for inf (and other non-finite IEEE 488 values)
          // so use values that exceed double and end up parsing as Inf when using
          // R's jsonlite::fromJSON()
          if (std::isfinite(y))
            std::cout << y;
          else
            std::cout << ((y > 0) ? "1e1024" : "-1e1024");
          std::cout << ",\"type\":\"real\"";
        }
        catch(const boost::bad_any_cast &) {
          try { std::cout << boost::any_cast<float>(x) << ",\"type\":\"real\""; }
          catch(const boost::bad_any_cast &) {
            try { std::cout << (boost::any_cast<bool>(x) ? "true" : "false") << ",\"type\":\"logical\""; }
            catch(const boost::bad_any_cast &) {
              try { std::cout << boost::any_cast<int>(x) << ",\"type\":\"integer\""; }
              catch(const boost::bad_any_cast &) {
                try { std::cout << boost::any_cast<unsigned>(x) << ",\"type\":\"unsigned integer\""; }
                catch(const boost::bad_any_cast &) {
                  std::cout << "null";
                }
              }
            }
          }
        }
        std::cout << "}";
      }
    }
    std::cout << "]}";
    return 0;
  }

  // maybe
    try {
      // create object that handles all receiver database transactions

      DB_Filer dbf (output_db, program_name, program_version, program_build_ts, bootnum, gps_min_dt);
      Tag_Candidate::set_filer(& dbf);

      Tag_Database * tag_db = 0;

      Node::init();

      // set up the data source
      Data_Source * pulses = 0;
      if (lotek) {
        if (src_sqlite) {
          // create tag_db here, since it won't be created below
          tag_db = new Tag_Database (tag_database, use_events);
          pulses = Data_Source::make_Lotek_source(& dbf, tag_db, default_freq, bootnum);
        } else {
          throw std::runtime_error("Must specify --src_sqlite with a Lotek data source");
        }
      } else if (src_sqlite) {
        pulses = Data_Source::make_SQLite_source(& dbf, bootnum);
      } else {
        pulses = Data_Source::make_SG_source(optind < argc ? argv[optind++] : "");
      }

      Tag_Foray foray;

      if (resume) {
        resume = Tag_Foray::resume(foray, pulses, bootnum);
        if (! resume) {
          std::cerr << "find_tags_motus: --resume failed" << std::endl;
        } else {
          std::cerr << "resumed successfully" << std::endl;
          tag_db = foray.tags;
          // Freq_Setting needs to know the set of nominal frequencies
          Freq_Setting::set_nominal_freqs(tag_db->get_nominal_freqs());
        }
      }
      if (! resume) {
        // either not asked to resume, or resume failed (e.g. no resume state saved)
        if (! tag_db)
          tag_db = new Tag_Database (tag_database, use_events);

        // Freq_Setting needs to know the set of nominal frequencies
        Freq_Setting::set_nominal_freqs(tag_db->get_nominal_freqs());

        foray = Tag_Foray(tag_db, pulses, default_freq, force_default_freq, min_dfreq, max_dfreq, max_pulse_rate, pulse_rate_window, min_bogus_spacing, unsigned_dfreq, pulses_only);
      }

      // record the commit hash from the meta database as an external parameter
      external_param_map[std::string("metadata_hash")] = tag_db->get_db_hash();

      dbf.add_param("default_freq", default_freq);
      dbf.add_param("force_default_freq", force_default_freq);
      dbf.add_param("use_events", use_events);
      dbf.add_param("burst_slop", burst_slop);
      dbf.add_param("burst_slop_expansion", burst_slop_expansion );
      dbf.add_param("pulses_to_confirm", pulses_to_confirm);
      dbf.add_param("signal_slop", sig_slop_dB);
      dbf.add_param("min_dfreq", min_dfreq);
      dbf.add_param("max_dfreq", max_dfreq);
      dbf.add_param("pulse_slop", pulse_slop);
      dbf.add_param("pulses_only", pulses_only);
      dbf.add_param("max_pulse_rate", max_pulse_rate );
      dbf.add_param("frequency_slop", frequency_slop);
      dbf.add_param("max_skipped_bursts", max_skipped_bursts);
      dbf.add_param("pulse_rate_window", pulse_rate_window);
      dbf.add_param("min_bogus_spacing", min_bogus_spacing);
      dbf.add_param("unsigned_dfreq", unsigned_dfreq);
      dbf.add_param("resume", resume);
      dbf.add_param("lotek", lotek);
      dbf.add_param("timestamp_wonkiness", timestamp_wonkiness);
      for (auto ii=external_param_map.begin(); ii != external_param_map.end(); ++ii)
        dbf.add_param(ii->first.c_str(), ii->second.c_str());

      // load any existing ambiguity mappings so that we don't generate new ambigIDs for the
      // same sets of ambiguous tags.  (This is where Ambiguity::ids is loaded, rather
      // than in Tag_Foray::resume, because we *always* want it).

      dbf.load_ambiguity();
#ifdef DEBUG
      std::cerr << "after resuming, nextID is " << Ambiguity::nextID << std::endl;
#endif

      if (graph_only) {
        foray.graph();
        exit(0);
      }
      if (test_only) {
        foray.test(); // throws if there's a problem
        std::cerr << "Ok\n";
        exit(0);
      }
      foray.start();
      std::cerr << "Max num candidates: " << Tag_Candidate::get_max_num_cands() << " at " << std::setprecision(14) << Tag_Candidate::get_max_cand_time() << "; now (" << foray.last_seen() << "): " << Tag_Candidate::get_num_cands() << std::endl;
      foray.pause();
    }
    catch (std::runtime_error& e) {
      std::cerr << e.what() << std::endl;
      exit(2);
    }
    std::cout << "Done." << std::endl;
}

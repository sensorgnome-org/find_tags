#include "Tag_Foray.hpp"
#include "SG_Record.hpp"

#include <string.h>
#include <sstream>
#include <time.h>
#include <cmath>

Tag_Foray::Tag_Foray () :  // default ctor for deserializing into
  line_no(0),   // line numbers reset even when resuming
  pulse_count(MAX_PORT_NUM + 1 + NUM_SPECIAL_PORTS),
  hist(0),      // we recreate history on resume
  tsBegin(0),
  prevHourBin(0)
{};

Tag_Foray::Tag_Foray (Tag_Database * tags, Data_Source *data, Frequency_MHz default_freq, bool force_default_freq, float min_dfreq, float max_dfreq, float max_pulse_rate, Gap pulse_rate_window, Gap min_bogus_spacing, bool unsigned_dfreq, bool pulses_only) :
  tags(tags),
  data(data),
  default_freq(default_freq),
  force_default_freq(force_default_freq),
  min_dfreq(min_dfreq),
  max_dfreq(max_dfreq),
  max_pulse_rate(max_pulse_rate),
  pulse_rate_window(pulse_rate_window),
  min_bogus_spacing(min_bogus_spacing),
  unsigned_dfreq(unsigned_dfreq),
  pulses_only(pulses_only),
  line_no(0),
  pulse_count(MAX_PORT_NUM + 1 + NUM_SPECIAL_PORTS),
  ts(0),
  pulse_slop(default_pulse_slop),
  burst_slop(default_burst_slop),
  burst_slop_expansion(default_burst_slop_expansion),
  max_skipped_bursts(default_max_skipped_bursts),
  hist(tags->get_history()),
  cron(hist->getTicker()),
  tsBegin(0),
  prevHourBin(0)
{
  // create one empty graph for each nominal frequency
  auto fs = tags->get_nominal_freqs();
  for (auto i = fs.begin(); i != fs.end(); ++i)
    graphs.insert(std::make_pair(*i, new Graph()));

  // set default frequencies for all ports
  for (auto i = -NUM_SPECIAL_PORTS; i < MAX_PORT_NUM; ++i)
    port_freq[i] = Freq_Setting(default_freq);
};


Tag_Foray::~Tag_Foray () {
  for (auto tfi = tag_finders.begin(); tfi != tag_finders.end(); ++tfi)
    delete (tfi->second);
};

void
Tag_Foray::set_default_pulse_slop_ms(float pulse_slop_ms) {
  default_pulse_slop = pulse_slop_ms / 1000.0;	// stored as seconds
};

void
Tag_Foray::set_default_burst_slop_ms(float burst_slop_ms) {
  default_burst_slop = burst_slop_ms / 1000.0;	// stored as seconds
};

void
Tag_Foray::set_default_burst_slop_expansion_ms(float burst_slop_expansion_ms) {
  default_burst_slop_expansion = burst_slop_expansion_ms / 1000.0;   // stored as seconds
};

void
Tag_Foray::set_default_max_skipped_bursts(unsigned int skip) {
  default_max_skipped_bursts = skip;
};

void
Tag_Foray::set_timestamp_wonkiness(unsigned int w) {
  timestamp_wonkiness = w;
};

void
Tag_Foray::start() {
  Tag_Candidate::ending_batch = false;

  cr = new Clock_Repair(data, &line_no, Tag_Candidate::filer);
  SG_Record r;

  if (! cr->get(r))
    return;  // no records, so nothing to do

  // the record returned by cr has a valid timestamp (the whole point of Clock_Repair)
  // so we can prune events corresponding to a tag having been activated and then died
  // *before* this first timestamp.  We allow for a 10 second reversal.
  hist->prune_deceased(r.ts - 10.0);

  // get the event iterator
  cron = hist->getTicker();

  bool have_record = true;
  for( ; have_record; have_record = cr->get(r)) {
    // get begin time, allowing for small time reversals (10 seconds)
    if (! tsBegin || (r.ts < tsBegin && r.ts >= tsBegin - 10.0)) {
      tsBegin = r.ts;
#ifdef ACTIVE_TAG_DIAGNOSTICS
      if (active_tag_dump_interval > 0.0 && next_active_tag_dump_time == 0.0) {
        dump_active_tags(tsBegin);
        next_active_tag_dump_time = tsBegin + active_tag_dump_interval;
      }
#endif // ACTIVE_TAG_DIAGNOSTICS
    }

    // skip record if it includes a time reversal of more than 10 seconds
    // (small time reversals are perfectly valid, and typically due to interleaved data
    // coming from different radios)
    if (r.ts - ts < -10.0)
      continue;

    ts = r.ts;
    switch (r.type) {
    case SG_Record::GPS:
      // GPS is not stuck, or Clock_Repair would have dropped the record
      // but only add it if r.v.lat and r.v.lon are actual numbers; r.v.alt might not be reported
      if (! (std::isnan(r.v.lat) || std::isnan(r.v.lon)))
        Tag_Candidate::filer->add_GPS_fix( r.ts, r.v.lat, r.v.lon, r.v.alt );
      break;

    case SG_Record::PARAM:

      Tag_Candidate::filer->add_recv_param( r.ts, r.port, r.v.param_flag, r.v.param_value, r.v.return_code, r.v.error);

      if (strcmp("-m", r.v.param_flag) || r.v.return_code || std::isnan(r.v.param_value)) {
        // ignore non-frequency parameter setting, or failed frequency setting
        continue;
      }

      if (! force_default_freq)
        port_freq[r.port] = Freq_Setting(r.v.param_value);
      continue;
      break;

    case SG_Record::PULSE:
      {
        // bump up the pulse count for the current hour bin

        double hourBin = round(r.ts / 3600);
        if (hourBin != prevHourBin) {
          if (prevHourBin > 0) {
            for (int i = 0; i < pulse_count.size(); ++i) {
              if (pulse_count[i] > 0) {
                Tag_Candidate::filer->add_pulse_count(prevHourBin, i - NUM_SPECIAL_PORTS, pulse_count[i]);
                pulse_count[i] = 0;
              }
            }
          }
          prevHourBin = hourBin;
        }

        if (r.port >= - NUM_SPECIAL_PORTS && r.port <= MAX_PORT_NUM)
          ++pulse_count[r.port + NUM_SPECIAL_PORTS];

        // skip this record if its offset frequency is out of bounds
        if (r.v.dfreq > max_dfreq || r.v.dfreq < min_dfreq)
          continue;

        Tag_Finder_Key key;

        if (! pulses_only) {
          // NB: cast r.port to work around optimization of passing reference
          // to packed struct

          // which tag finder should this pulse be passed to?  There is at
          // least a tag finder on each port, and possibly more than one if
          // the listening frequency is changing.

          key = Tag_Finder_Key((short int) r.port, port_freq[r.port].f_kHz);

          // if there isn't already an appropriate Tag_Finder, create it
          if (! tag_finders.count(key)) {
            Tag_Finder *newtf;
            std::ostringstream prefix;
            prefix << r.port << ",";
            if (max_pulse_rate > 0)
              newtf = new Rate_Limiting_Tag_Finder(this, key.second, tags->get_tags_at_freq(key.second), graphs[key.second], pulse_rate_window, max_pulse_rate, min_bogus_spacing, prefix.str());
            else
              newtf = new Tag_Finder(this, key.second, tags->get_tags_at_freq(key.second), graphs[key.second], prefix.str());
            tag_finders[key] = newtf;
#ifdef DEBUG3
            std::cerr << "Interval Tree for " << prefix.str() << std::endl;
            newtf->graph.get_root()->dump(std::cerr);
            std::cerr << "Burst slop expansion is " << Tag_Finder::default_burst_slop_expansion << std::endl;
#endif
          };
        }

        if (r.v.dfreq < 0 && unsigned_dfreq)
          r.v.dfreq = - r.v.dfreq;

        // create a pulse object from this record
        Pulse p = Pulse::make(r.ts, r.v.dfreq, r.v.sig, r.v.noise, port_freq[r.port].f_MHz);

        // process any tag events up to this point in time

        while (cron.ts() <= p.ts)
          process_event(cron.get());

#ifdef ACTIVE_TAG_DIAGNOSTICS
        if (active_tag_dump_interval > 0.0 && p.ts > next_active_tag_dump_time) {
          dump_active_tags(p.ts);
          // bump up next active tag dump time by at least one interval, and by enough
          // intervals to be greater than p.ts
          next_active_tag_dump_time += active_tag_dump_interval * (1.0 + floor((p.ts - next_active_tag_dump_time) / active_tag_dump_interval));
        }
#endif // ACTIVE_TAG_DIAGNOSTICS

        if (pulses_only) {
          Tag_Candidate::filer->add_pulse(r.port, p);
        } else {
#ifdef DEBUG2
          std::cerr << p.ts << ": Key: " << r.port << ", " << port_freq[r.port].f_kHz << std::endl;
#endif
          tag_finders[key]->process(p);
#ifdef DEBUG3
          tag_finders[key]->dump(r.ts);
#endif
        }
      }
      break;
    case SG_Record::EXTENSION:
      {
        // for future extension: in-band commands
      }
      break;
    default:
      break;
    }
  }
  // record pulse counts from the last hour bin

  for (int i = 0; i < pulse_count.size(); ++i)
    if (pulse_count[i] > 0)
      Tag_Candidate::filer->add_pulse_count(prevHourBin, i - NUM_SPECIAL_PORTS, pulse_count[i]);
};

void
Tag_Foray::process_event(Event e) {
  auto t = e.tag;
  auto fs = Freq_Setting::as_Nominal_Frequency_kHz(t->freq);
  Graph * g = graphs[fs];
  switch (e.code) {
  case Event::E_ACTIVATE:
    {
      if (t->active)
        return;
      auto rv = g->addTag(t, pulse_slop, burst_slop / 4.0, (1 + max_skipped_bursts) * 4.0, timestamp_wonkiness);
#ifdef DEBUG2
      g->viz();
#endif
      // in case an ambiguity proxy tag was generated, mark that as active
      // A proxy tag might have already existed because it was read from the database
      // but in that case, it won't have been marked active.
      // That would be a problem if another tag was to be added to the ambiguity
      // later (the assert in Graph::find() fails)
      rv.second && (rv.second->active = true);

      for (auto i = tag_finders.begin(); i != tag_finders.end(); ++i)
        if (i->first.second == fs)
          i->second->tag_added(rv);
      t->active = true;
#ifdef DEBUG2
      std::cerr << "Activating " << t->motusID << "=" << (void *) t << std::endl;
#endif
    }
    break;
  case Event::E_DEACTIVATE:
    {
      if (! t->active)
        return;
      auto rv = g->delTag(t);
#ifdef DEBUG2
      g->viz();
#endif
      // if we removed one ambiguity and replaced it with a reduced one
      // (or with a real tag), make sure the removed ambiguity is marked
      // as inactive, and the remaining tag or ambiguity is actve
      rv.first && (rv.first->active = false);
      rv.second && (rv.second->active = true);

      for (auto i = tag_finders.begin(); i != tag_finders.end(); ++i)
        if (i->first.second == fs)
          i->second->tag_removed(rv);
      t->active = false;
#ifdef DEBUG2
      std::cerr << "Deactivating " << t->motusID << "=" << (void *) t << std::endl;
#endif
    }
    break;
  default:
    std::cerr << "Warning: Unknown event code " << e.code << " for tag " << t->motusID << std::endl;
  };
}

void
Tag_Foray::test() {
  // try build tag finders for each nominal frequency

  Freq_Set fs = tags->get_nominal_freqs();
  for (Freq_Set :: iterator it = fs.begin(); it != fs.end(); ++it) {
    Tag_Finder_Key key(0, *it);
    std::string prefix="p";
    Tag_Finder *newtf;
    port_freq[0] = Freq_Setting(*it / 1000.0);
    if (max_pulse_rate > 0)
      newtf = new Rate_Limiting_Tag_Finder(this, key.second, tags->get_tags_at_freq(key.second), graphs[*it], pulse_rate_window, max_pulse_rate, min_bogus_spacing, prefix);
    else
      newtf = new Tag_Finder(this, key.second, tags->get_tags_at_freq(key.second), graphs[*it], prefix);
    // FIXME: do something to test validity?
    delete newtf;
  }
}

void
Tag_Foray::graph() {
  // plot the DFA graphs for the given tag database, one per nominal frequency

  Timestamp t = time_now();
  while (cron.ts() < t) // process all events to this point in time
    process_event(cron.get());

  Freq_Set fs = tags->get_nominal_freqs();
  for (Freq_Set :: iterator it = fs.begin(); it != fs.end(); ++it) {
    Tag_Finder_Key key(0, *it);
    std::string prefix="p";
    Tag_Finder *newtf;
    port_freq[0] = Freq_Setting(*it / 1000.0);
    if (max_pulse_rate > 0)
      newtf = new Rate_Limiting_Tag_Finder(this, key.second, tags->get_tags_at_freq(key.second), graphs[*it], pulse_rate_window, max_pulse_rate, min_bogus_spacing, prefix);
    else
      newtf = new Tag_Finder(this, key.second, tags->get_tags_at_freq(key.second), graphs[*it], prefix);
    newtf->graph->viz();
  }
}

Gap Tag_Foray::default_pulse_slop = 0.0015; // 1.5 ms
Gap Tag_Foray::default_burst_slop = 0.010; // 10 ms
Gap Tag_Foray::default_burst_slop_expansion = 0.001; // 1ms = 1 part in 10000 for 10s BI
unsigned int Tag_Foray::default_max_skipped_bursts = 60;
unsigned int Tag_Foray::timestamp_wonkiness = 0;// maximum seconds of clock jump size in Lotek .DTA data files

Tag_Foray::Run_Cand_Counter Tag_Foray::num_cands_with_run_id_ = Run_Cand_Counter();

#ifdef ACTIVE_TAG_DIAGNOSTICS
double Tag_Foray::active_tag_dump_interval = 0; // seconds between dumps of active tag IDs (if > 0)
double Tag_Foray::next_active_tag_dump_time = 0; // real (data) time of next active tag ID dump
#endif // ACTIVE_TAG_DIAGNOSTICS

void
Tag_Foray::pause() {
  // serialize and save state of the tag finder
  // this is the top-level of the serializer,
  // so we dump class static members from here.

  // before doing so, reap candidates from all tag finders so
  // we finish runs which have expired. (needed e.g. when no
  // pulses have been received for an antenna in a long time)
  for (auto tfi = tag_finders.begin(); tfi != tag_finders.end(); ++tfi)
    tfi->second->reap(ts);

  // Now when destructors are called for remaining (non-expired)
  // tag candidates, we do *not* want to actually end the run,
  // as it might continue in the next batch of data.  Indicate
  // this.

  Tag_Candidate::ending_batch = true;
  std::ostringstream ofs;

  Tag_Candidate::filer->end_batch(tsBegin, ts);

  Ambiguity::record_ids();

  {
    // block to ensure oa dtor is called
    boost::archive::binary_oarchive oa(ofs);

    // Ambiguity (serialized structures)
    oa << make_nvp("abm", Ambiguity::abm);
    oa << make_nvp("nextID", Ambiguity::nextID);

    // Tag_Foray
    oa << make_nvp("default_pulse_slop", Tag_Foray::default_pulse_slop);
    oa << make_nvp("default_burst_slop", Tag_Foray::default_burst_slop);
    oa << make_nvp("default_burst_slop_expansion", Tag_Foray::default_burst_slop_expansion);
    oa << make_nvp("default_max_skipped_bursts", Tag_Foray::default_max_skipped_bursts);

    oa << make_nvp("num_cands_with_run_id_", Tag_Foray::num_cands_with_run_id_);

    // Freq_Setting
    oa << make_nvp("nominal_freqs", Freq_Setting::nominal_freqs);

    // Pulse
    oa << make_nvp("count", Pulse::count);

    // Node
    oa << make_nvp("_numNodes", Node::_numNodes);
    oa << make_nvp("_numLinks", Node::_numLinks);
    oa << make_nvp("maxLabel", Node::maxLabel);
    oa << make_nvp("_empty", Node::_empty);

    // Set
    oa << make_nvp("_numSets", Set::_numSets);
    oa << make_nvp("maxLabel", Set::maxLabel);
    oa << make_nvp("_empty", Set::_empty);

    // Tag_Candidate
    oa << make_nvp("freq_slop_kHz", Tag_Candidate::freq_slop_kHz);
    oa << make_nvp("sig_slop_dB", Tag_Candidate::sig_slop_dB);
    oa << make_nvp("pulses_to_confirm_id", Tag_Candidate::pulses_to_confirm_id);
    oa << make_nvp("num_cands", Tag_Candidate::num_cands);

    // dynamic members of all classes
    serialize(oa, SERIALIZATION_VERSION);

    // data source
    data->serialize(oa, SERIALIZATION_VERSION);

  }
  // record this state

  Tag_Candidate::filer->
    save_findtags_state( ts,                   // last timestamp parsed from input
                         time_now(),           // time now
                         ofs.str(),            // serialized state
                         SERIALIZATION_VERSION // version
                         );

};

bool
Tag_Foray::resume(Tag_Foray &tf, Data_Source *data, long long bootnum) {
  Timestamp paused;
  Timestamp lastLineTS;
  std::string blob;

  int ser_ver; // serialization version of saved data

  if (! Tag_Candidate::filer->
      load_findtags_state( bootnum,
                           paused,
                           lastLineTS,
                           blob,                      // serialized state
                           SERIALIZATION_VERSION,
                           ser_ver
                           ))
    return false;

  std::istringstream ifs (blob);
  boost::archive::binary_iarchive ia(ifs);

  // Ambiguity (serialized structures)
  ia >> make_nvp("abm", Ambiguity::abm);
  ia >> make_nvp("nextID", Ambiguity::nextID);

  // Tag_Foray
  ia >> make_nvp("default_pulse_slop", Tag_Foray::default_pulse_slop);
  ia >> make_nvp("default_burst_slop", Tag_Foray::default_burst_slop);
  ia >> make_nvp("default_burst_slop_expansion", Tag_Foray::default_burst_slop_expansion);
  ia >> make_nvp("default_max_skipped_bursts", Tag_Foray::default_max_skipped_bursts);

  ia >> make_nvp("num_cands_with_run_id_", Tag_Foray::num_cands_with_run_id_);

  // Freq_Setting
  ia >> make_nvp("nominal_freqs", Freq_Setting::nominal_freqs);

  // Pulse
  ia >> make_nvp("count", Pulse::count);

  // Node
  ia >> make_nvp("_numNodes", Node::_numNodes);
  ia >> make_nvp("_numLinks", Node::_numLinks);
  ia >> make_nvp("maxLabel", Node::maxLabel);
  ia >> make_nvp("_empty", Node::_empty);

  // Set
  ia >> make_nvp("_numSets", Set::_numSets);
  ia >> make_nvp("maxLabel", Set::maxLabel);
  ia >> make_nvp("_empty", Set::_empty);

  // Tag_Candidate
  ia >> make_nvp("freq_slop_kHz", Tag_Candidate::freq_slop_kHz);
  ia >> make_nvp("sig_slop_dB", Tag_Candidate::sig_slop_dB);
  ia >> make_nvp("pulses_to_confirm_id", Tag_Candidate::pulses_to_confirm_id);
  ia >> make_nvp("num_cands", Tag_Candidate::num_cands);

  // dynamic members of all classes
  tf.serialize(ia, ser_ver);

  // data source deserialization happens into the
  // new data source
  tf.data = data;

  data->serialize(ia, ser_ver);

  return true;
};

int
Tag_Foray::num_cands_with_run_id (DB_Filer::Run_ID rid, int delta) {
  if (rid == 0)
    return 0;
  auto i = num_cands_with_run_id_.find(rid);
  if (i == num_cands_with_run_id_.end()) {
    // rid not present
    if (delta == 0)
      return 0;
    if (delta < 0)
      throw std::runtime_error("Tried to reduce count of cands with run_id already at 0.");
    num_cands_with_run_id_.insert(std::make_pair(rid, delta));
    return delta;
  } else {
    if (delta == 0)
      return i->second;
    i->second += delta;
    if (i->second > 0)
      return i->second;
    num_cands_with_run_id_.erase(i);
    return(0);
  }
};

#ifdef ACTIVE_TAG_DIAGNOSTICS
void
Tag_Foray::dump_active_tags(double ts) {
  std::cout << std::setprecision(14);
  for (auto tfi = tag_finders.begin(); tfi != tag_finders.end(); ++tfi) {
    Tag_Finder_Key key = tfi->first;
    std::cout << ts << ',' << key.first << ',' << key.second * 0.001 << ',';
    tfi->second->graph->dumpActiveTags();
    std::cout << std::endl;
  }
  std::cout << std::setprecision(6);
};

void
Tag_Foray::set_active_tag_dump_interval(double t) {
  active_tag_dump_interval = t;
};
#endif //ACTIVE_TAG_DIAGNOSTICS

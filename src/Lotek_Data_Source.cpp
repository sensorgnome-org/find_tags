#include "Lotek_Data_Source.hpp"
#include <sstream>
#include <cstdio>

Lotek_Data_Source::Lotek_Data_Source(DB_Filer * db, Tag_Database *tdb, Frequency_MHz defFreq, int bootnum) :
  db(db),
  done(false),
  sgbuf(),
  latestInputTS(0),
  bootnum(bootnum)
{
  // generate the map from (codeSet, ID) -> Tag *
  auto kf = tdb->get_nominal_freqs();
  for (auto f = kf.begin(); f != kf.end(); ++ f) {
    auto tags = tdb->get_tags_at_freq(*f);
    for (auto t = tags->begin(); t != tags->end(); ++t) {
      Tag & tg = **t;
      tcode.insert(std::make_pair(std::make_pair(tg.codeSet, tg.mfgID), & tg.gaps));
    }
  }
  // generate the vector of antenna frequencies

  // NOTE: indexes in this array are offset by one (e.g. antFreq[1] is
  // the frequency for port 0) to accommodate the -1 value that
  // represents "A1+A2+A3+A4" in antFreq[0]

  antFreq = std::vector < Frequency_MHz > ( MAX_ANTENNAS, defFreq);

  // start the db reader
  db->start_DTAtags_reader(0, bootnum);

};

bool
Lotek_Data_Source::getInputLine() {
  if (done)
    return false;
  if (! db->get_DTAtags_record(dtar)) {
    done = true;
    return false;
  }
  return true;
 };

bool
Lotek_Data_Source::getline(char * buf, int maxLen) {

  for(;;) {
    // wait until either the true input is done, or
    // we have sufficiently old data in sgbuf (i.e. lines which
    // are old enough to be guaranteed (by the value of MAX_LEAD_SECONDS)
    // that no older data will be generated from subsequent input records
    auto i = sgbuf.begin();
    if (i != sgbuf.end() && i->first + MAX_LEAD_SECONDS <= latestInputTS) {
      // easy case - there's a sufficiently old line in the buffer
      strncpy(buf, i->second.c_str(), maxLen);
#ifdef DEBUG
      std::cerr << i->second << std::endl;
#endif
      sgbuf.erase(i);
      return true;
    }
    // no lines in sgbuf are sufficiently old.  If there are no true
    // input lines left, we're done (we save insufficiently old
    // sgbuf lines until the next time the algorithm is run with
    // subsequent data, because there might be pulses from there
    // which precede some of those in the current sgbuf.

    if (done)
      return false;

    // typical case; no sufficiently old lines left in sgbuf, but we haven't
    // reached EOF on input

    if (getInputLine())
      translateLine();

    // loop around until eof or a line is found
  }
};

void
Lotek_Data_Source::translateLine()
{
  // add appropriate SG lines to the buffer for a given Lotek line
  // The lotek line is in components of class field dtar
  //
  // Algorithm: if the current tag detection frequency does not match the
  // value for the current antenna, then issue a frequency-change command.
  // Then, issue a set of 4 pulse records, representing the coded ID.

  // clean up weird lotek antenna naming: either [0-9] or A[0-9] or AH[0-9] or "A1+A2+A3+A4" (the master antenna on SRX 800)
  int len = strlen(dtar.antName);
  if (len <= 3) {
    dtar.ant = dtar.antName[len - 1] - '0';
  } else {
    dtar.ant = -1;  // aka A1+A2+A3+A4
  }

  // output a GPS fix, if the tag record has valid lat and lon; DTA files don't report altitude, so report as nan
  if (!(std::isnan(dtar.lat) || std::isnan(dtar.lon))) {
    /* a GPS fix line like:
       G,1458001712,44.34021,-66.118733333,21.6
    */
    std::ostringstream gpsRec;
    gpsRec << "G," << std::setprecision(14) << dtar.ts << std::setprecision(8) << "," << dtar.lat << "," << dtar.lon << ",nan";
    sgbuf.insert(std::make_pair(dtar.ts, gpsRec.str()));
  }

  latestInputTS = dtar.ts;
  if (dtar.freq != antFreq[dtar.ant + 1]) {
    antFreq[dtar.ant + 1] = dtar.freq;
    std::ostringstream freqRec;
    // make frequency setting record like: S,1366227448.192,5,-m,166.376,0,

    freqRec << "S," << std::setprecision(14) << dtar.ts << std::setprecision(6) << "," << dtar.ant << ",-m," << dtar.freq << ",0,";
    sgbuf.insert(std::make_pair(dtar.ts, freqRec.str()));
  }

  bool validTag = dtar.id != 999;
  tcode_t::iterator tt; // will hold time gaps for a valid tag

  if (validTag) {
    auto csid = std::make_pair(dtar.codeSet, dtar.id);
    tt = tcode.find(csid);
    if (tt == tcode.end()) {
      if (! warned.count(csid)) {
        std::cerr << "Ignoring record: no known tag with ID " << dtar.id << " in codeset " << dtar.codeSet << std::endl;
        warned.insert(csid);
      }
      validTag = false;
    }
  }

  if (! validTag) {
    // generate a single pulse record, so that the downstream code
    // records activity on this antenna at this time.  We use values of -999
    // (effectively sentinels) for dfreq, sig, and noise, to make sure
    // this pulse doesn't end up as part of any real detection.
    std::ostringstream pRec;
    pRec << "p" << dtar.ant << "," << std::setprecision(14) << dtar.ts << std::setprecision(3) << ",-999,-999,-999";
    sgbuf.insert(std::make_pair(dtar.ts, pRec.str()));
    return;
  }

  auto gg = tt->second;
  // generate a record for each tag pulse

  for(auto i = gg->begin(); i != gg->end(); ++i) {
    std::ostringstream pRec;
    // "%hd,%lf,%f,%f,%f", &port_num, &ts, &dfreq, &sig, &noise)) {
    // we use dfreq=4 to put it at the usual nominal SG frequency (i.e. funcube is tuned 4 kHz
    // below nominal, so dfreq=4 means a tag on nominal)
    pRec << "p" << dtar.ant << "," << std::setprecision(14) << dtar.ts << std::setprecision(3) << ",4," << dtar.sig << ",-96";
    sgbuf.insert(std::make_pair(dtar.ts, pRec.str()));
    dtar.ts += *i; // NB: the last gap takes us to the next burst, so is not actually used
  }
}

void
Lotek_Data_Source::rewind() {
  db->rewind_DTAtags_reader();
};

// ugly macro because I couldn't figure out how to make this work
// properly with templates.

// Note: we only serialize what's needed to resume
// processing buffered lines.  Tcode will be reconstructed
// from the tag database provided, as we don't want
// to be saving another copy of tag signatures (and the
// copy saved might be saved with motus in the future).

#define SERIALIZE_FUN_BODY \
   ar & BOOST_SERIALIZATION_NVP( sgbuf );                       \
   ar & BOOST_SERIALIZATION_NVP( latestInputTS );               \
   ar & BOOST_SERIALIZATION_NVP( antFreq );                     \
   ar & BOOST_SERIALIZATION_NVP( warned );

void
Lotek_Data_Source::serialize(boost::archive::binary_iarchive & ar, const unsigned int version) {

  SERIALIZE_FUN_BODY;

};

void
Lotek_Data_Source::serialize(boost::archive::binary_oarchive & ar, const unsigned int version) {

  SERIALIZE_FUN_BODY;

};

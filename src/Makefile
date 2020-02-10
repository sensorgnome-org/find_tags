## Makefile for linux

CXX=g++

## PROFILING FLAGS (uncomment to enable profiling)
##PROFILING=-g3 -pg -fno-omit-frame-pointer

## DEBUG FLAGS:
##CPPFLAGS=-Wall -Wno-sign-compare -g3  -std=c++11 $(PROFILING) -DPROGRAM_VERSION=$(PROGRAM_VERSION) -DPROGRAM_BUILD_TS=$(PROGRAM_BUILD_TS) -I/usr/local/include/boost_1.60 -DDEBUG
## add -DDEBUG2 and -DDEBUG3 for more extensive debug output
## To build with active tag diagnostics, add -DACTIVE_TAG_DIAGNOSTICS.  That gives you the -a option
## to find_tags_motus (do find_tags_motus --help after this rebuild to see details)

## PRODUCTION FLAGS:
CPPFLAGS=-Wall -Wno-sign-compare -g -O3 -std=c++11 $(PROFILING) -DPROGRAM_VERSION=$(PROGRAM_VERSION) -DPROGRAM_BUILD_TS=$(PROGRAM_BUILD_TS)

LDFLAGS=-ldl -lrt -lboost_serialization -lboost_program_options -lsqlite3
PROGRAM_VERSION=\""$(shell git describe)\""
PROGRAM_BUILD_TS=$(shell date +%s)

all: find_tags_motus testAddRemoveTag# dfa_graph.pdf ## find_tags_unifile

install: find_tags_motus
	sudo cp find_tags_motus /sgm/bin
	strip /sgm/bin/find_tags_motus

## separate installation for find_tags_unifile, which must be built
## after doing `git checkout find_tags_unifile`.  Yes, ugly.

install_find_tags_unifile: find_tags_unifile
	sudo cp find_tags_unifile /sgm/bin
	strip /sgm/bin/find_tags_unifile

OBJS=                            \
   Ambiguity.o			 \
   Clock_Pinner.o		 \
   Clock_Repair.o		 \
   Data_Source.o		 \
   DB_Filer.o			 \
   Freq_Setting.o		 \
   GPS_Validator.o               \
   Graph.o			 \
   History.o			 \
   Lotek_Data_Source.o		 \
   Node.o			 \
   Pulse.o			 \
   Rate_Limiting_Tag_Finder.o	 \
   Set.o			 \
   SG_File_Data_Source.o	 \
   SG_Record.o                   \
   SG_SQLite_Data_Source.o	 \
   Tag_Candidate.o		 \
   Tag_Database.o		 \
   Tag_Finder.o			 \
   Tag_Foray.o			 \
   Tag.o			 \
   Ticker.o			 \
# END OF OBJS

clean:
	rm -f $(OBJS) find_tags_unifile find_tags_motus  find_tags_motus.o  testAddRemoveTag.o testAddRemoveTag

Ambiguity.o: Ambiguity.hpp Ambiguity.cpp

Clock_Pinner.o: Clock_Pinner.hpp Clock_Pinner.cpp

Clock_Repair.o: Clock_Repair.hpp Clock_Repair.cpp Clock_Pinner.hpp GPS_Validator.hpp

Data_Source.o: Data_Source.hpp find_tags_common.hpp

DB_Filer.o: DB_Filer.cpp DB_Filer.hpp find_tags_common.hpp

DFA_Graph.o: DFA_Graph.cpp DFA_Graph.hpp find_tags_common.hpp

DFA_Node.o: DFA_Node.cpp DFA_Node.hpp find_tags_common.hpp

Freq_Setting.o: Freq_Setting.cpp Freq_Setting.hpp find_tags_common.hpp

GPS_Validator.o: GPS_Validator.hpp GPS_Validator.cpp

Graph.o: Graph.hpp Graph.cpp Set.hpp Node.hpp Tag.hpp find_tags_common.hpp

History.o: Event.hpp History.hpp History.cpp

Lotek_Data_Source.o: Lotek_Data_Source.hpp Data_Source.hpp find_tags_common.hpp

Node.o: Node.hpp Node.cpp Tag.hpp find_tags_common.hpp

Pulse.o: Pulse.cpp Pulse.hpp find_tags_common.hpp

Rate_Limiting_Tag_Finder.o: Rate_Limiting_Tag_Finder.hpp find_tags_common.hpp

Set.o: Set.hpp find_tags_common.hpp

SG_File_Data_Source.o: SG_File_Data_Source.hpp Data_Source.hpp find_tags_common.hpp

SG_Record.o: SG_Record.cpp SG_Record.hpp

SG_SQLite_Data_Source.o: SG_SQLite_Data_Source.hpp Data_Source.hpp find_tags_common.hpp DB_Filer.hpp

Tag_Candidate.o: Tag_Candidate.hpp Tag_Candidate.cpp Tag_Finder.hpp Bounded_Range.hpp find_tags_common.hpp

Tag_Database.o: Tag_Database.cpp Tag_Database.hpp find_tags_common.hpp

Tag_Finder.o: Tag_Finder.hpp Tag_Finder.cpp Tag_Candidate.hpp find_tags_common.hpp

Tag_Foray.o: Tag_Foray.hpp Tag_Foray.cpp find_tags_common.hpp DB_Filer.hpp SG_Record.hpp

Tag.o: Tag.hpp Tag.cpp find_tags_common.hpp

Ticker.o: Ticker.hpp Ticker.cpp History.hpp

find_tags_unifile.o: find_tags_unifile.cpp find_tags_common.hpp Freq_Setting.hpp DFA_Node.hpp DFA_Graph.hpp Tag.hpp Tag_Database.hpp Pulse.hpp Burst_Params.hpp Bounded_Range.hpp Tag_Candidate.hpp Tag_Finder.hpp Rate_Limiting_Tag_Finder.hpp Tag_Foray.hpp

find_tags_unifile: Freq_Setting.o DFA_Node.o DFA_Graph.o Tag.o Tag_Database.o Pulse.o Tag_Candidate.o Tag_Finder.o Rate_Limiting_Tag_Finder.o find_tags_unifile.o Tag_Foray.o
	g++ $(PROFILING) -o find_tags_unifile $^ $(LDFLAGS)

find_tags_motus.o: find_tags_motus.cpp find_tags_common.hpp Freq_Setting.hpp Tag.hpp Tag_Database.hpp Pulse.hpp Burst_Params.hpp Bounded_Range.hpp Tag_Candidate.hpp Tag_Finder.hpp Rate_Limiting_Tag_Finder.hpp Tag_Foray.hpp Ambiguity.hpp Lotek_Data_Source.hpp SG_File_Data_Source.hpp SG_SQLite_Data_Source.hpp

find_tags_motus: $(OBJS) find_tags_motus.o
	g++ $(PROFILING) -o find_tags_motus $^ $(LDFLAGS)

ftmdbg: $(OBJS) find_tags_motus.o
	g++ $(PROFILING) -o ftmdbg $^ $(LDFLAGS)

testAddRemoveTag.o: testAddRemoveTag.cpp find_tags_unifile.cpp find_tags_common.hpp Freq_Setting.hpp Tag.hpp Tag_Database.hpp Pulse.hpp Burst_Params.hpp Bounded_Range.hpp Tag_Candidate.hpp Tag_Finder.hpp Rate_Limiting_Tag_Finder.hpp Tag_Foray.hpp

## Note: to make testAddRemoteTag, Graph.cpp must be compiled with -DDEBUG
testAddRemoveTag: testAddRemoveTag.o Ambiguity.o  Freq_Setting.o  History.o  Pulse.o Set.o Tag_Candidate.o  Tag_Finder.o  Tag.o Ticker.o DB_Filer.o Graph.o Node.o Rate_Limiting_Tag_Finder.o Tag_Database.o Tag_Foray.o Data_Source.o Lotek_Data_Source.o SG_File_Data_Source.o Clock_Repair.o Clock_Pinner.o GPS_Validator.o SG_Record.o SG_SQLite_Data_Source.o
	g++ $(PROFILING) -o testAddRemoveTag $^ $(LDFLAGS)

#ifndef GRAPH_HPP
#define GRAPH_HPP

#include "find_tags_common.hpp"
#include "Node.hpp"
#include "Ambiguity.hpp"

class Graph {
  // the graph representing a DFA for the NDFA full-burst recognition
  // problem on a set of known tags
  
protected:
  
  Node * _root;
  std::string vizPrefix;
  int numViz;

  // map from sets to nodes
  std::unordered_map < Set *, Node *, hashSet, SetEqual > setToNode; 

  // stamp; each time a recursive algorithm is run, the stamp value is
  // first increased; as the graph is traversed, nodes are stamped with
  // the new value to indicate they have been visited.  Avoids having
  // to reset a "visited" flag across the graph at the start of each
  // run of the algorithm.  When the stamp value wraps back to 0, all
  // nodes get stamped with 0 and the new stamp value is set to 1.
  int stamp;

  Ambiguity amb; // manage tag ambiguity sets

public:

  Graph(std::string vizPrefix = "graph");
  Node * root();
  std::pair < Tag *, Tag * > addTag(Tag * tag, double tol, double timeFuzz, double maxTime);  //!< add a tag to the tree, handling ambiguity
  std::pair < Tag *, Tag * >  delTag(Tag * tag, double tol, double timeFuzz, double maxTime); //!< remove a tag from the tree, handling ambiguity
  void renTag(Tag *t1, Tag *t2);//!< "rename" tag t1 to tag t2
  Tag * find(Tag * tag);
  void viz();
  void dumpSetToNode();
  void validateSetToNode();

protected:

  void newStamp(); //!< update the stamp value; restamp all nodes when it wraps.

  void resetAllStamps();

  void mapSet( Set * s, Node * n);

  void unmapSet ( Set * s);

  void insert (const TagPhase &t);

  void erase (const TagPhase &t);

  void insert (GapRanges & gr, TagPhase p);

  void erase (GapRanges & gr, TagPhase p);

  bool hasEdge ( Node *n, Gap b, TagPhase p);

  Node::Edges::iterator ensureEdge ( Node *n, Gap b);

  void linkNode (Node *n);

  void unlinkNode (Node *n); 

  void augmentEdge(Node::Edges::iterator i, TagPhase p);
  
  void reduceEdge(Node::Edges::iterator i, TagPhase p);

  void dropEdgeIfExtra(Node * n, Node::Edges::iterator i);

  void insert (Node *n, GapRanges & gr, TagPhase p);
    
  void insertRec (GapRanges & gr, TagPhase tFrom, TagPhase tTo);

  void insertRec (Node *n, GapRanges & gr, TagPhase tFrom, TagPhase tTo);

  void erase (Node *n, GapRanges & gr, TagPhase tp);

  void eraseRec (GapRanges & gr, TagPhase tpFrom, TagPhase tpTo);

  void eraseRec (Node *n, GapRanges & gr, TagPhase tpFrom, TagPhase tpTo);

  void renTagRec(Node * n, Tag *t1, Tag *t2); //!< rename a tag from t1 to t2, starting at node n, and recursing

  void _addTag(Tag * tag, double tol, double timeFuzz, double maxTime);  //!< add a tag to the tree, but no handling of ambiguity
  void _delTag(Tag * tag, double tol, double timeFuzz, double maxTime); //!< remove a tag from the tree, but no handling of ambiguity

public: 
  
  template<class Archive>
  void serialize(Archive & ar, const unsigned int version) {
    ar & _root;
    ar & vizPrefix;
    ar & numViz;
    ar & stamp;
    ar & amb;
  };
};

#endif // GRAPH_HPP
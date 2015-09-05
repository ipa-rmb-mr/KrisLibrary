#include "TriMeshOperators.h"
#include <math/SVDecomposition.h>
#include <math/matrix.h>
#include <utils/combination.h>
#include <set>

namespace Meshing {

///Returns true if the vertex is a boundary vertex
bool IncidentTriangleOrdering(const TriMeshWithTopology& mesh,int v,vector<list<int> >& triStrips)
{
  Assert(!mesh.incidentTris.empty());
  Assert(!mesh.triNeighbors.empty());
  set<int> incidentTris;
  for(size_t i=0;i<mesh.incidentTris[v].size();i++) {
    int t=mesh.incidentTris[v][i];
    incidentTris.insert(t);
  }
  triStrips.resize(0);
  while(!incidentTris.empty()) {
    int t=*incidentTris.begin();
    triStrips.resize(triStrips.size()+1);
    triStrips.back().push_back(t);
    incidentTris.erase(incidentTris.begin());
    //go forward (CCW)
    int t0=t;
    for(;;) {  //iterate on t
      int n=CCWNeighbor(mesh,t,v);
      if(n == t0) return false;
      if(n>=0) {
	triStrips.back().push_back(n);
	set<int>::iterator i=incidentTris.find(n);
	Assert(i!=incidentTris.end());
	incidentTris.erase(i);
	t=n;
      }
      else break;
    }
    t=t0;  //set t back to the start
    //go backward (CW)
    for(;;) {  //iterate on t
      int n=CWNeighbor(mesh,t,v);
      if(n == t0) return false;
      if(n>=0) {
	triStrips.back().push_front(n);
	set<int>::iterator i=incidentTris.find(n);
	Assert(i!=incidentTris.end());
	incidentTris.erase(i);
	t=n;
      }
      else break;
    }
  }
  return true;
}

Real VertexGaussianCurvature(const TriMeshWithTopology& mesh,int v)
{
  Assert(!mesh.incidentTris.empty());
  vector<list<int> > strips;
  bool res=IncidentTriangleOrdering(mesh,v,strips);
  if(!res) { //non-boundary vertex
    Assert(strips.size()==1);
    Real sumAngles=0;
    for(size_t i=0;i<mesh.incidentTris[v].size();i++) {
      //add the angle of the outgoing edges about v
      int t=mesh.incidentTris[v][i];
      const IntTriple& tri=mesh.tris[t];
      int vindex=tri.getIndex(v);
      Assert(vindex!=-1);
      int v1,v2;
      tri.getCompliment(vindex,v1,v2);
      Vector3 e1=mesh.verts[v1]-mesh.verts[v];
      Vector3 e2=mesh.verts[v2]-mesh.verts[v];
      Real a = Angle(e1,e2);
      sumAngles += a;
    }
    return 3.0*(TwoPi-sumAngles)/IncidentTriangleArea(mesh,v);
  }
  else {
    Real sum=0;
    for(size_t s=0;s<strips.size();s++) {
      Real subtendedAngle;
      int t0=*strips[s].begin();
      int tn=*(--strips[s].end());
      Assert(CWNeighbor(mesh,t0,v) <0);
      Assert(CCWNeighbor(mesh,tn,v) <0);
      int v0=CWAdjacentVertex(mesh,t0,v);
      int vn=CCWAdjacentVertex(mesh,tn,v);
      subtendedAngle = Angle(mesh.verts[vn]-mesh.verts[v],mesh.verts[v0]-mesh.verts[v]);

      Real sumAngles=0;
      for(list<int>::const_iterator i=strips[s].begin();i!=strips[s].end();i++) {
	int t=*i;
	//get vertices on t opposite v
	const IntTriple& tri=mesh.tris[t];
	int vindex=tri.getIndex(v);
	Assert(vindex!=-1);
	int v1,v2;
	tri.getCompliment(vindex,v1,v2);
	Vector3 e1=mesh.verts[v1]-mesh.verts[v];
	Vector3 e2=mesh.verts[v2]-mesh.verts[v];
	Real a = Angle(e1,e2);
	sumAngles += a;
      }
      sum += (subtendedAngle-sumAngles)*subtendedAngle/TwoPi;
    }
    return 3.0*sum/IncidentTriangleArea(mesh,v);
  }
}

Real VertexAbsMeanCurvature(const TriMeshWithTopology& mesh,int v)
{
  if(mesh.incidentTris[v].empty()) return 0;
  Real sum=0;
  //go around triangles in CCW order
  for(size_t i=0;i<mesh.incidentTris[v].size();i++) {
    int t1=mesh.incidentTris[v][i];
    int t2=CCWNeighbor(mesh,t1,v);
    if(t2==-1) 
      continue;
    Vector3 n1=mesh.TriangleNormal(t1);
    Vector3 n2=mesh.TriangleNormal(t2);
    int e=mesh.triNeighbors[t1].getIndex(t2);  //edge index
    Assert(e!=-1);
    int v1,v2;
    mesh.tris[t1].getCompliment(e,v1,v2);
    Assert(v1 == v || v2 == v);
    if(v2 == v) swap(v1,v2);
    Real edgeLen = mesh.verts[v].distance(mesh.verts[v2]);
    //dihedral angle
    Real dihedral=Angle(n1,n2);
    sum += edgeLen*Abs(dihedral);
  }
  return 3.0*sum*0.25/IncidentTriangleArea(mesh,v);
}

//solves the problem 
//[a1 a2 a3][x] = [amount]
//[b1 b2 b3][y]   [amount]
//[c1 b2 b3][z]   [amount]
//in exact (or failing that for numerical reasons, least squares) form
Vector3 Mat3Solve(const Vector3& a,const Vector3& b,const Vector3& c,Real amount)
{
  Matrix3 C,Cinv;
  C.setRow1(a);
  C.setRow2(b);
  C.setRow3(c);
  if(!Cinv.setInverse(C)) {
    //numerical error? colinear
    RobustSVD<Real> svd;
    Matrix Cm(3,3);
    for(int p=0;p<3;p++)
      for(int q=0;q<3;q++)
	Cm(p,q) = C(p,q);
    if(!svd.set(Cm)) {
      //yikes, what's going on here?
      return amount*(a+b+c)/3.0;
    }
    else {
      Matrix Cminv;
      svd.getInverse(Cminv);
      for(int p=0;p<3;p++)
	for(int q=0;q<3;q++)
	  Cinv(p,q) = Cminv(p,q);
    }
  }
  return Cinv*Vector3(amount,amount,amount);
}

int ApproximateShrink(TriMeshWithTopology& mesh,Real amount)
{
  if(mesh.incidentTris.empty()) 
    mesh.CalcIncidentTris();
  vector<Vector3> n(mesh.tris.size());
  for(size_t i=0;i<mesh.tris.size();i++) 
    n[i] = mesh.TriangleNormal(i);
  vector<Vector3> ni;  //temporary
  for(size_t i=0;i<mesh.verts.size();i++) {
    //set up a min norm program
    //min ||x|| s.t.
    //ni^T x + amount <= 0 for all triangles i in incident(v)
    //(let this be Ax <= b)
    //In other words, with lagrange multipliers m
    //  x + A^T m = 0
    //  Ax <= b
    //  m^T(Ax-b)=0
    //  m >= 0
    //For the active multipliers with rows C of A selected
    //  x + C^T m = 0
    //  C x = b
    //so b = -CC^T m => m=-(CC^T)^-1*b
    //=> x = C^T (CC^T)^-1*b
    ni.resize(0);
    for(size_t j=0;j<mesh.incidentTris[i].size();j++) {
      bool duplicate = false;
      for(size_t k=0;k<ni.size();k++) {
	if(ni[k].isEqual(n[mesh.incidentTris[i][j]],Epsilon)) {
	  duplicate = true;
	  break;
	}
      }
      if(!duplicate)
	ni.push_back(n[mesh.incidentTris[i][j]]);
    }
    //cout<<"ApproximateShrink "<<i<<" "<<ni.size()<<endl;
    if(ni.empty()) continue;
    else if(ni.size() == 1) {
      //shift inward
      mesh.verts[i] -= amount*ni[0];
      cout<<"  "<<ni[0]<<endl;
    }
    else if(ni.size() == 2) {
      //shift inward, solve analytically
      Matrix2 CtC;
      CtC(0,0) = ni[0].dot(ni[0]);
      CtC(0,1) = CtC(1,0) = ni[0].dot(ni[1]);
      CtC(1,1) = ni[1].dot(ni[1]);
      Matrix2 CtCinv;
      if(!CtCinv.setInverse(CtC)) {
	//numerical error? colinear
	mesh.verts[i] -= amount*ni[0];
      }
      else {
	Vector2 coeffs = CtCinv*Vector2(amount,amount);
	mesh.verts[i] -= coeffs[0]*ni[0] + coeffs[1]*ni[1];
	//cout<<"  "<<coeffs[0]*ni[0] + coeffs[1]*ni[1]<<endl;
	//cout<<"  coeffs "<<coeffs[0]<<endl;
      }
    }
    else if(ni.size()==3) {
      mesh.verts[i] -= Mat3Solve(ni[0],ni[1],ni[2],amount);
      //cout<<"  "<<Mat3Solve(ni[0],ni[1],ni[2],amount)<<endl;
    }
    else {
      //simple method: loop through all triples and pick the deepest
      Vector3 deepest;
      Real maxdepth = 0;
      vector<int> triple(3);
      FirstCombination(triple,ni.size());
      do {
	Vector3 v = Mat3Solve(ni[triple[0]],ni[triple[1]],ni[triple[2]],amount);
	Real md = amount;
	for(size_t j=0;j<ni.size();j++) 
	  md=Min(md,ni[j].dot(v));
	if(md > maxdepth) {
	  maxdepth = md;
	  deepest = v;
	}
      }
      while(!NextCombination(triple,ni.size()));
      //cout<<"  Deepest is "<<deepest<<" with depth "<<maxdepth<<endl;
      mesh.verts[i] -= deepest;
    }
  }
  //check for flips
  int numFlipped = 0;
  for(size_t i=0;i<mesh.tris.size();i++)
    if(mesh.TriangleNormal(i).dot(n[i]) < 0) numFlipped++;
  return numFlipped;
}

} //namespace Meshing

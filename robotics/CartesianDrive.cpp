#include "CartesianDrive.h"
#include "IKFunctions.h"
#include "Rotation.h"
using namespace std;

bool IsFiniteV(const Vector3& x)
{
  return ::IsFinite(x.x) && ::IsFinite(x.y) && ::IsFinite(x.z);
}

CartesianDriveSolver::CartesianDriveSolver(RobotDynamics3D* _robot)
  :robot(_robot),positionTolerance(1e-3),rotationTolerance(1e-3),ikSolveTolerance(0),ikSolveIters(100),driveSpeedAdjustment(1.0)
{
}

void CartesianDriveSolver::Init(const Config& q,int link)
{
  vector<int> _links(1,link);
  Init(q,_links);
}

void CartesianDriveSolver::Init(const Config& q,int link,const Vector3& endEffectorPosition)
{
  vector<int> _links(1,link);
  vector<Vector3> offsets(1,endEffectorPosition);
  Init(q,_links,offsets);
}

void CartesianDriveSolver::Init(const Config& q,const vector<int>& _links)
{
  vector<Vector3> endEffectorOffsets;
  Init(q,_links,endEffectorOffsets);
}

void CartesianDriveSolver::Init(const Config& q,const vector<int>& _links,const vector<Vector3>& _endEffectorOffsets)
{
  vector<int> baseLinks;
  Init(q,_links,baseLinks,_endEffectorOffsets);
}

void CartesianDriveSolver::Init(const Config& q,const vector<int>& _links,const vector<int>& _baseLinks,const vector<Vector3>& _endEffectorOffsets)
{
  Assert(q.n == robot->q.n);
  if(!_endEffectorOffsets.empty()) Assert(_endEffectorOffsets.size() == _links.size());
  if(!_baseLinks.empty()) Assert(_baseLinks.size() == _links.size());
  links = _links;
  endEffectorOffsets = _endEffectorOffsets;
  if(endEffectorOffsets.empty())
    endEffectorOffsets.resize(_links.size(),Vector3(0,0,0));
  baseLinks = _baseLinks;
  if(baseLinks.empty())
    baseLinks.resize(_links.size(),-1);

  robot->UpdateConfig(q);
  driveSpeedAdjustment = 1.0;
  driveTransforms.resize(_links.size());
  for(size_t i=0;i<_links.size();i++) {
    Assert(_links[i] >= 0 && _links[i] < robot->q.n);
    Assert(baseLinks[i] >= -1 && baseLinks[i] < robot->q.n);
    if(baseLinks[i] < 0) {
      driveTransforms[i].R = robot->links[links[i]].T_World.R;
      driveTransforms[i].t = robot->links[links[i]].T_World*endEffectorOffsets[i];
    }
    else {
      driveTransforms[i].R.mulTransposeA(robot->links[baseLinks[i]].T_World.R,robot->links[links[i]].T_World.R); 
      robot->links[baseLinks[i]].T_World.mulInverse(robot->links[links[i]].T_World*endEffectorOffsets[i],driveTransforms[i].t);
    }
  }
}

Real CartesianDriveSolver::Drive(const Config& qcur,const Vector3& driveAngVel,const Vector3& driveVel,Real dt,Config& qout)
{
  Assert(links.size() == 1);
  vector<Vector3> driveAngVels(1,driveAngVel);
  vector<Vector3> driveVels(1,driveVel);
  return Drive(qcur,driveAngVels,driveVels,dt,qout);
}

Real CartesianDriveSolver::Drive(const Config& qcur,const vector<Vector3>& driveAngVel,const vector<Vector3>& driveVel,Real dt,Config& qout)
{
  Assert(qcur.n == robot->q.n);
  Assert(driveAngVel.size() == driveVel.size());
  Assert(links.size() == driveVel.size());
  qout = qcur;

  //update drive transforms
  bool anyNonzero = false;
  for(size_t i=0;i<driveVel.size();i++)
    if(!driveVel[i].isZero() || !driveAngVel[i].isZero()) {
      anyNonzero = true;
      break;
    }
  //zero velocity, stop at current without computation.
  if(!anyNonzero) return 1.0;

  //update drive transforms
  robot->UpdateConfig(qcur);
  vector<RigidTransform> originalTransforms(links.size());
  for(size_t i=0;i<links.size();i++) {
    originalTransforms[i].R = robot->links[links[i]].T_World.R;
    originalTransforms[i].t = robot->links[links[i]].T_World*endEffectorOffsets[i];
  }

  //advance the desired cartesian goals
  Real amount = dt * driveSpeedAdjustment;
  vector<RigidTransform> desiredTransforms(links.size());
  for(size_t i=0;i<links.size();i++) {
    if(IsFiniteV(driveVel[i])) 
      desiredTransforms[i].t = amount*driveVel[i] + driveTransforms[i].t;
    if(IsFiniteV(driveAngVel[i])) {
      Matrix3 increment;
      MomentRotation m(driveAngVel[i] * amount);
      m.getMatrix(increment);
      desiredTransforms[i].R = increment * driveTransforms[i].R;
    }
  }

  //set up IK parameters: active dofs, IKGoals
  ArrayMapping tempActiveDofs;
  vector<IKGoal> tempGoals;
  if(ikGoals.empty()) {
    tempGoals.resize(links.size());
    for(size_t i=0;i<links.size();i++) {
      if(IsFiniteV(driveVel[i])) {
	tempGoals[i].localPosition = endEffectorOffsets[i];
	tempGoals[i].SetFixedPosition(desiredTransforms[i].t);
      }
      else {
	tempGoals[i].SetFreePosition();
      }
      if(IsFiniteV(driveAngVel[i])) 
	tempGoals[i].SetFixedRotation(desiredTransforms[i].R);
      else
	tempGoals[i].SetFreeRotation();
    }
  }
  else {
    FatalError("Can't set up custom IK goals yet");
  }
  if(activeDofs.empty())
    GetDefaultIKDofs(*robot,tempGoals,tempActiveDofs);
  else
    tempActiveDofs.mapping = activeDofs;

  ///limit the joint movement by joint limits and velocity
  Vector tempqmax,tempqmin;
  if(qmin.empty()) tempqmin = robot->qMin;
  else tempqmin = qmin;
  if(qmax.empty()) tempqmax = robot->qMax;
  else tempqmax = qmax;
  for(size_t i=0;i<tempActiveDofs.mapping.size();i++) {
    int k=tempActiveDofs.mapping[i];
    if(vmax.empty())
      tempqmax[k] = Min(tempqmax[k],qcur[k]+dt*robot->velMax[k]);
    else
      tempqmax[k] = Min(tempqmax[k],qcur[k]+dt*vmax[k]);
    if(vmin.empty())
      tempqmin[k] = Max(tempqmin[k],qcur[k]-dt*robot->velMax[k]);
    else
      tempqmin[k] = Max(tempqmin[k],qcur[k]+dt*vmin[k]);
  }

  //Do the IK solve
  RobotIKFunction function(*robot);
  function.activeDofs = tempActiveDofs;
  for(size_t i=0;i<tempGoals.size();i++) {
    //function.UseIK(goal);
    IKGoalFunction* goalfunc = new IKGoalFunction(*robot,tempGoals[i],function.activeDofs);
    if(IsFinite(positionTolerance) && IsFinite(rotationTolerance)) {
      goalfunc->rotationScale = positionTolerance/(positionTolerance+rotationTolerance);
      goalfunc->positionScale = rotationTolerance/(positionTolerance+rotationTolerance);
    }
    else if(!IsFinite(positionTolerance) && !IsFinite(rotationTolerance)) {
      //keep equal tolerance
    }
    else {
      goalfunc->rotationScale = Min(positionTolerance,rotationTolerance)/rotationTolerance;
      goalfunc->positionScale = Min(positionTolerance,rotationTolerance)/positionTolerance;
    }
    //printf("Position scale %g, rotation scale %g\n",goalfunc->positionScale,goalfunc->rotationScale);
    function.functions.push_back(goalfunc);
  }

  //evaluate starting quality
  Vector x0(tempActiveDofs.mapping.size()),err0(function.NumDimensions());
  function.GetState(x0);
  function(x0,err0);
  Real quality0 = err0.normSquared();

  Real tolerance = ikSolveTolerance;
  if(ikSolveTolerance == 0) tolerance = Min(1e-6,Min(positionTolerance,rotationTolerance)/Sqrt(3.0*links.size()));
  int iters = ikSolveIters;
  int verbose = 0;
  RobotIKSolver solver(function);
  solver.UseJointLimits(tempqmin,tempqmax);
  solver.solver.verbose = verbose;
  bool res = solver.Solve(tolerance,iters);
  //bool res = ::SolveIK(function,tolerance,iters,verbose);
  if(!tempqmin.empty()) {
    //check joint limits
    for(size_t i=0;i<tempActiveDofs.mapping.size();i++) {
      int k=tempActiveDofs.mapping[i];
      if(robot->q[k] < tempqmin[k] || robot->q[k] > tempqmin[k]) {
        //the IK solver normalizer doesn't care about absolute
        //values for joints that wrap around 2pi
	if(tempqmin[k] <= robot->q[k] + TwoPi && robot->q[k] + TwoPi <= tempqmax[k])
	  robot->q[k] += TwoPi;
	else if(tempqmin[k] <= robot->q[k] - TwoPi && robot->q[k] - TwoPi <= tempqmax[k])
	  robot->q[k] -= TwoPi;
	else {
	  printf("Warning, result from IK solve is out of bounds: index %d, %g <= %g <= %g\n",k,tempqmin[k],robot->q[k],tempqmax[k]);
	  robot->q[k] = Clamp(robot->q[k],tempqmin[k],tempqmax[k]);
	  robot->UpdateFrames();
	}
      }
    }
  }

  //now evaluate quality of the solve
  function.GetState(x0);
  function(x0,err0);
  Real qualityAfter = err0.normSquared();
  if(qualityAfter > quality0) {
    printf("Solve failed: original configuration was better\n");
    res = false;
  }
  else {
    for(size_t i=0;i<links.size();i++) {
      //test constraints
      Vector3 perr(0.0),rerr(0.0);
      tempGoals[i].GetError(robot->links[links[i]].T_World,perr,rerr);
      if(perr.norm() < positionTolerance && rerr.norm() < rotationTolerance) {
        res = true;
      }
      else {
        res = false;
        printf("Position error: %g, rotation error: %g not under tolerances %g, %g\n",perr.norm(),rerr.norm(),positionTolerance,rotationTolerance);
        printf("Solve tolerance %g, result %d\n",tolerance,(int)res);
      }
    }
  }

  if(res) {
    //success!
    for(size_t i=0;i<tempActiveDofs.mapping.size();i++) {
      int k=tempActiveDofs.mapping[i];
      qout[k] = robot->q[k];
      Assert(tempqmin[k]<=robot->q[k] && robot->q[k]<=tempqmax[k]);
    }

    //now advance the driven transforms
    if(links.size()==1)
      robot->UpdateSelectedFrames(links[0]);
    else
      robot->UpdateFrames();
    vector<RigidTransform> achievedTransforms;
    //figure out how much to drive along screw
    Real numerator = 0;  //< this will get sum of distance * screws
    Real denominator = 0;  //< this will get sum of |screw|^2 for all screws
    //result will be numerator / denominator
    for(size_t i=0;i<links.size();i++) {
      achievedTransforms[i].R = robot->links[links[i]].T_World.R;
      achievedTransforms[i].t = robot->links[links[i]].T_World*endEffectorOffsets[i];
      //cout<<"  Solved limb transform: "<<achievedTransforms[i].t<<endl;
      
      //adjust drive transform along screw to minimize distance to the achieved transform      
      if(IsFiniteV(driveVel[i])) {
	Vector3 trel = achievedTransforms[i].t - driveTransforms[i].t;
	Vector3 axis = driveVel[i] / Max(driveVel[i].length(),Epsilon);
	Real ut = driveVel[i].length();
	Real tdistance = trel.dot(axis);
	//cout<<"  translation vector"<<trel<<endl;
	//printf("  Translation amount: %g\n",tdistance);
	tdistance = Clamp(tdistance,0.0,dt*driveVel[i].length());
	numerator += ut*tdistance;
	denominator += Sqr(ut);
      }
      if(IsFiniteV(driveAngVel[i])) {
	Matrix3 Rrel;
	Rrel.mulTransposeB(achievedTransforms[i].R,driveTransforms[i].R);
	Vector3 rotaxis = driveAngVel[i] / Max(driveAngVel[i].length(),Epsilon);
	Real Rdistance = AxisRotationMagnitude(Rrel,rotaxis);
	//printf("  Rotation amount: %g (desired %g)\n",Rdistance,dt*driveAngVel[i].length());
	Rdistance = Clamp(Rdistance,0.0,dt*driveAngVel[i].length());
	Real uR = driveAngVel[i].length();
	numerator += uR*Rdistance;
	denominator += Sqr(uR);
      }
    }
    Real distance = numerator / Max(denominator,Epsilon);
    //printf("Distance %g\n",distance);
      
    //computed error-minimizing distance along screw motion
    for(size_t i=0;i<links.size();i++) {
      if(IsFiniteV(driveVel[i])) 
	driveTransforms[i].t.madd(driveVel[i],distance);
      else
	driveTransforms[i].t = achievedTransforms[i].t;
      if(IsFiniteV(driveAngVel[i])) {
	MomentRotation m;
	Matrix3 Rincrement;
	m.set(driveAngVel[i]*distance);
	m.getMatrix(Rincrement);
	driveTransforms[i].R = Rincrement * driveTransforms[i].R;
	NormalizeRotation(driveTransforms[i].R);
      }
      else
	driveTransforms[i].R = achievedTransforms[i].R;
    }

    //increase drive velocity
    if(driveSpeedAdjustment < 1.0)
      driveSpeedAdjustment += 0.1;

    return distance / dt;
  }
  else {
    driveSpeedAdjustment -= 0.1;
    printf("  CartesianDriveSolver: Solve failed, next trying with amount %g\n",driveSpeedAdjustment);
    return 0;
  }
  if(driveSpeedAdjustment <= Epsilon) {
    //don't adjust drive transform
    printf("  CartesianDriveSolver: IK solve failed completely.  Must restart.\n");
    return 0;
  }
}

void CartesianDriveSolver::GetTrajectory(const Config& qcur,
					 const vector<Vector3>& angVel,
					 const vector<Vector3>& vel,
					 Real dt,
					 int numSteps,
					 vector<Config>& qout,
					 bool reset)
{
  vector<RigidTransform> startDriveTransforms;
  Real startDriveSpeedAdjustment;
  if(reset) {
    startDriveTransforms = driveTransforms;
    startDriveSpeedAdjustment = driveSpeedAdjustment;
  }

  qout.resize(numSteps + 1);
  qout[0] = qcur;
  for(int i=0;i<numSteps;i++) {
    Real frac = Drive(qout[i],angVel,vel,dt,qout[i+1]);
    if(frac == 0) {
      //done, just stop
      while(i < numSteps) {
	qout[i+1] = qout[i];
	i++;
      }
      break;
    }
  }  

  if(reset) {
    swap(startDriveTransforms,driveTransforms);
    swap(startDriveSpeedAdjustment,driveSpeedAdjustment);
  }
}

// This file is part of Tasks.
//
// Tasks is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Tasks is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with Tasks.  If not, see <http://www.gnu.org/licenses/>.

// associated header
#include "QPMotionConstr.h"

// includes
// Eigen
#include <unsupported/Eigen/Polynomials>

// RBDyn
#include <RBDyn/MultiBody.h>
#include <RBDyn/MultiBodyConfig.h>

// Tasks
#include "Bounds.h"
#include "utils.h"


namespace tasks
{

namespace qp
{

/**
	*															PositiveLambda
	*/

PositiveLambda::PositiveLambda():
	lambdaBegin_(-1),
	XL_(),
	XU_(),
	cont_()
{ }


void PositiveLambda::updateNrVars(const std::vector<rbd::MultiBody>& /* mbs */,
	const SolverData& data)
{
	lambdaBegin_ = data.lambdaBegin();

	XL_.setConstant(data.totalLambda(), 0.);
	XU_.setConstant(data.totalLambda(), std::numeric_limits<double>::infinity());

	cont_.clear();
	const std::vector<BilateralContact>& allC = data.allContacts();
	for(std::size_t i = 0; i < allC.size(); ++i)
	{
		cont_.push_back({allC[i].contactId,
										data.lambdaBegin(int(i)),
										allC[i].nrLambda()});
	}
}


void PositiveLambda::update(const std::vector<rbd::MultiBody>& /* mbs */,
	const std::vector<rbd::MultiBodyConfig>& /* mbc */,
	const SolverData& /* data */)
{ }


std::string PositiveLambda::nameBound() const
{
	return "PositiveLambda";
}


std::string PositiveLambda::descBound(const std::vector<rbd::MultiBody>& mbs,
	int line)
{
	std::ostringstream oss;

	for(const ContactData& cd: cont_)
	{
		int begin = cd.lambdaBegin - lambdaBegin_;
		int end = begin + cd.nrLambda;
		if(line >= begin && line < end)
		{
			const rbd::MultiBody& mb1 = mbs[cd.cId.r1Index];
			const rbd::MultiBody& mb2 = mbs[cd.cId.r2Index];
			oss << "Body 1: " << mb1.body(mb1.bodyIndexById(cd.cId.r1BodyId)).name() << std::endl;
			oss << "Body 2: " << mb2.body(mb2.bodyIndexById(cd.cId.r2BodyId)).name() << std::endl;
			break;
		}
	}

	return oss.str();
}


int PositiveLambda::beginVar() const
{
	return lambdaBegin_;
}


const Eigen::VectorXd& PositiveLambda::Lower() const
{
	return XL_;
}


const Eigen::VectorXd& PositiveLambda::Upper() const
{
	return XU_;
}


/**
	*															MotionConstrCommon
	*/


MotionConstrCommon::ContactData::ContactData(const rbd::MultiBody& mb,
	int bId, int lB,
	std::vector<Eigen::Vector3d> pts,
	const std::vector<FrictionCone>& cones):
	bodyIndex(),
	lambdaBegin(lB),
	jac(mb, bId),
	points(std::move(pts)),
	generators(cones.size()),
	jacTrans(6, jac.dof())
{
	bodyIndex = jac.jointsPath().back();
	for(std::size_t i = 0; i < cones.size(); ++i)
	{
		generators[i].resize(3, cones[i].generators.size());
		for(std::size_t j = 0; j < cones[i].generators.size(); ++j)
		{
			generators[i].col(j) = cones[i].generators[j];
		}
	}
}


MotionConstrCommon::MotionConstrCommon(const std::vector<rbd::MultiBody>& mbs,
	int robotIndex):
	robotIndex_(robotIndex),
	alphaDBegin_(-1),
	nrDof_(mbs[robotIndex_].nrDof()),
	lambdaBegin_(-1),
	fd_(mbs[robotIndex_]),
	fullJac_(6, nrDof_),
	cont_(),
	curTorque_(nrDof_),
	A_(),
	AL_(nrDof_),
	AU_(nrDof_)
{
	assert(std::size_t(robotIndex_) < mbs.size() && robotIndex_ >= 0);
}


void MotionConstrCommon::computeTorque(const Eigen::VectorXd& alphaD, const Eigen::VectorXd& lambda)
{
	curTorque_ = fd_.H()*alphaD.segment(alphaDBegin_, nrDof_);
	curTorque_ += fd_.C();
	curTorque_ += A_.block(0, lambdaBegin_, nrDof_, A_.cols() - lambdaBegin_)*lambda;
}


const Eigen::VectorXd& MotionConstrCommon::torque() const
{
	return curTorque_;
}


void MotionConstrCommon::torque(const std::vector<rbd::MultiBody>& mbs,
	std::vector<rbd::MultiBodyConfig>& mbcs) const
{
	const rbd::MultiBody& mb = mbs[robotIndex_];
	rbd::MultiBodyConfig& mbc = mbcs[robotIndex_];

	int pos = mb.joint(0).dof();
	for(std::size_t i = 1; i < mbc.jointTorque.size(); ++i)
	{
		for(double& d: mbc.jointTorque[i])
		{
			d = curTorque_(pos);
			++pos;
		}
	}
}


void MotionConstrCommon::updateNrVars(const std::vector<rbd::MultiBody>& mbs,
	const SolverData& data)
{
	const rbd::MultiBody& mb = mbs[robotIndex_];

	alphaDBegin_ = data.alphaDBegin(robotIndex_);
	lambdaBegin_ = data.lambdaBegin();

	cont_.clear();
	const auto& cCont = data.allContacts();
	for(std::size_t i = 0; i < cCont.size(); ++i)
	{
		const BilateralContact& c = cCont[i];
		if(robotIndex_ == c.contactId.r1Index)
		{
			cont_.emplace_back(mb, c.contactId.r1BodyId, data.lambdaBegin(int(i)),
				c.r1Points, c.r1Cones);
		}
		// we don't use else to manage self contact on the robot
		if(robotIndex_ == c.contactId.r2Index)
		{
			cont_.emplace_back(mb, c.contactId.r2BodyId, data.lambdaBegin(int(i)),
				c.r2Points, c.r2Cones);
		}
	}

	A_.setZero(nrDof_, data.nrVars());
}


void MotionConstrCommon::computeMatrix(const std::vector<rbd::MultiBody>& mbs,
	const std::vector<rbd::MultiBodyConfig>& mbcs)
{
	using namespace Eigen;

	const rbd::MultiBody& mb = mbs[robotIndex_];
	const rbd::MultiBodyConfig& mbc = mbcs[robotIndex_];

	fd_.computeH(mb, mbc);
	fd_.computeC(mb, mbc);

	// tauMin -C <= H*alphaD - J^t G lambda <= tauMax - C

	// fill inertia matrix part
	A_.block(0, alphaDBegin_, nrDof_, nrDof_) = fd_.H();

	for(std::size_t i = 0; i < cont_.size(); ++i)
	{
		const MatrixXd& jac = cont_[i].jac.bodyJacobian(mb, mbc);

		ContactData& cd = cont_[i];
		int lambdaOffset = 0;
		for(std::size_t j = 0; j < cd.points.size(); ++j)
		{
			/// @todo don't translate the 6 rows
			/// @todo don't full the 6 rows
			/// @todo apply generator on the not full jac
			cd.jac.translateBodyJacobian(jac, mbc, cd.points[j], cd.jacTrans);
			cd.jac.fullJacobian(mb, cd.jacTrans, fullJac_);

			A_.block(0, cd.lambdaBegin + lambdaOffset,
				nrDof_, cd.generators[j].cols()).noalias() =
					-fullJac_.block(3, 0, 3, fullJac_.cols()).transpose()*
						cd.generators[j];
			lambdaOffset += int(cd.generators[j].cols());
		}
	}

	// BEq = -C
	AL_ = -fd_.C();
	AU_ = -fd_.C();
}


int MotionConstrCommon::maxGenInEq() const
{
	return int(A_.rows());
}


const Eigen::MatrixXd& MotionConstrCommon::AGenInEq() const
{
	return A_;
}


const Eigen::VectorXd& MotionConstrCommon::LowerGenInEq() const
{
	return AL_;
}


const Eigen::VectorXd& MotionConstrCommon::UpperGenInEq() const
{
	return AU_;
}


std::string MotionConstrCommon::nameGenInEq() const
{
	return "MotionConstr";
}


std::string MotionConstrCommon::descGenInEq(const std::vector<rbd::MultiBody>& mbs,
	int line)
{
	int jIndex = findJointFromVector(mbs[robotIndex_], line, true);
	return std::string("Joint: ") + mbs[robotIndex_].joint(jIndex).name();
}


/**
	*															MotionConstr
	*/


MotionConstr::MotionConstr(const std::vector<rbd::MultiBody>& mbs,
	int robotIndex, const TorqueBound& tb):
	MotionConstrCommon(mbs, robotIndex),
	torqueL_(mbs[robotIndex].nrDof()),
	torqueU_(mbs[robotIndex].nrDof())
{
	rbd::paramToVector(tb.lTorqueBound, torqueL_);
	rbd::paramToVector(tb.uTorqueBound, torqueU_);
}


void MotionConstr::update(const std::vector<rbd::MultiBody>& mbs,
	const std::vector<rbd::MultiBodyConfig>& mbcs,
	const SolverData& /* data */)
{
	computeMatrix(mbs, mbcs);

	AL_.head(torqueL_.rows()) += torqueL_;
	AU_.head(torqueU_.rows()) += torqueU_;
}


/**
	*															MotionSpringConstr
	*/


MotionSpringConstr::MotionSpringConstr(
	const std::vector<rbd::MultiBody>& mbs, int robotIndex,
	const TorqueBound& tb, const std::vector<SpringJoint>& springs):
	MotionConstr(mbs, robotIndex, tb),
	springs_()
{
	const rbd::MultiBody& mb = mbs[robotIndex_];
	for(const SpringJoint& sj: springs)
	{
		int index = mb.jointIndexById(sj.jointId);
		int posInDof = mb.jointPosInDof(index);
		springs_.push_back({index, posInDof, sj.K, sj.C, sj.O});
	}
}


void MotionSpringConstr::update(const std::vector<rbd::MultiBody>& mbs,
	const std::vector<rbd::MultiBodyConfig>& mbcs,
	const SolverData& /* data */)
{
	const rbd::MultiBodyConfig& mbc = mbcs[robotIndex_];

	computeMatrix(mbs, mbcs);

	for(const SpringJointData& sj: springs_)
	{
		double spring = mbc.q[sj.index][0]*sj.K + mbc.alpha[sj.index][0]*sj.C + sj.O;
		torqueL_(sj.posInDof) = -spring;
		torqueU_(sj.posInDof) = -spring;
	}

	AL_.head(torqueL_.rows()) += torqueL_;
	AU_.head(torqueU_.rows()) += torqueU_;
}


/**
	*															MotionPolyConstr
	*/


MotionPolyConstr::MotionPolyConstr(const std::vector<rbd::MultiBody>& mbs,
	int robotIndex, const PolyTorqueBound& ptb):
	MotionConstrCommon(mbs, robotIndex),
	torqueL_(),
	torqueU_(),
	jointIndex_()
{
	const rbd::MultiBody& mb = mbs[robotIndex_];

	for(int i = 0; i < mb.nrJoints(); ++i)
	{
		if(mb.joint(i).dof() == 1)
		{
			torqueL_.push_back(ptb.lPolyTorqueBound[i][0]);
			torqueU_.push_back(ptb.uPolyTorqueBound[i][0]);
			jointIndex_.push_back(i);
		}
	}
}


void MotionPolyConstr::update(const std::vector<rbd::MultiBody>& mbs,
	const std::vector<rbd::MultiBodyConfig>& mbcs,
	const SolverData& /* data */)
{
	const rbd::MultiBody& mb = mbs[robotIndex_];
	const rbd::MultiBodyConfig& mbc = mbcs[robotIndex_];

	computeMatrix(mbs, mbcs);

	for(std::size_t i = 0; i < jointIndex_.size(); ++i)
	{
		int index = jointIndex_[i];
		int dofPos = mb.jointPosInDof(index);
		AL_(dofPos) += Eigen::poly_eval(torqueL_[i], mbc.q[index][0]);
		AU_(dofPos) += Eigen::poly_eval(torqueU_[i], mbc.q[index][0]);
	}
}


} // namespace qp

} // namespace tasks

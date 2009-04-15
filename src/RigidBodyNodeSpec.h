#ifndef SimTK_SIMBODY_RIGID_BODY_NODE_SPEC_H_
#define SimTK_SIMBODY_RIGID_BODY_NODE_SPEC_H_

/* -------------------------------------------------------------------------- *
 *                      SimTK Core: SimTK Simbody(tm)                         *
 * -------------------------------------------------------------------------- *
 * This is part of the SimTK Core biosimulation toolkit originating from      *
 * Simbios, the NIH National Center for Physics-Based Simulation of           *
 * Biological Structures at Stanford, funded under the NIH Roadmap for        *
 * Medical Research, grant U54 GM072970. See https://simtk.org.               *
 *                                                                            *
 * Portions copyright (c) 2005-9 Stanford University and the Authors.         *
 * Authors: Michael Sherman                                                   *
 * Contributors:                                                              *
 *    Charles Schwieters (NIH): wrote the public domain IVM code from which   *
 *                              this was derived.                             *
 *    Peter Eastman: wrote the Euler Angle<->Quaternion conversion            *
 *                                                                            *
 * Permission is hereby granted, free of charge, to any person obtaining a    *
 * copy of this software and associated documentation files (the "Software"), *
 * to deal in the Software without restriction, including without limitation  *
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,   *
 * and/or sell copies of the Software, and to permit persons to whom the      *
 * Software is furnished to do so, subject to the following conditions:       *
 *                                                                            *
 * The above copyright notice and this permission notice shall be included in *
 * all copies or substantial portions of the Software.                        *
 *                                                                            *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR *
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   *
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    *
 * THE AUTHORS, CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,    *
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR      *
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE  *
 * USE OR OTHER DEALINGS IN THE SOFTWARE.                                     *
 * -------------------------------------------------------------------------- */


/**@file
 * This file contains just the templatized class RigidBodyNodeSpec<dof> which
 * is used as the base class for most of the RigidBodyNodes (that is, the
 * implementations of Mobilizers). The only exceptions are nodes whose 
 * mobilizers provide no degrees of freedom -- Ground and Weld.
 *
 * This file contains all the multibody mechanics method declarations that 
 * involve a single body and its mobilizer (inboard joint), that is, one node 
 * in the multibody tree. These methods constitute the inner loops of the 
 * multibody calculations, and much suffering is undergone here to make them 
 * run fast. In particular most calculations are templatized by the number of 
 * mobilities, so that compile-time sizes are known for everything.
 *
 * Most methods here expect to be called in a particular order during traversal 
 * of the tree -- either base to tip or tip to base.
 */

#include "SimbodyMatterSubsystemRep.h"
#include "RigidBodyNode.h"

/**
 * This still-abstract class is a skeleton implementation of a built-in mobilizer, with the
 * number of mobilities (dofs, u's) specified as a template parameter. That way all
 * the code that is common except for the dimensionality of the mobilizer can be
 * written once, and the compiler generates specific implementations for each of the
 * six possible dimensionalities (1-6 mobilities). Each implementation works only on
 * fixed size Vec<> and Mat<> types, so can use very high speed inline operators.
 */
template<int dof>
class RigidBodyNodeSpec : public RigidBodyNode {
public:
    RigidBodyNodeSpec(const MassProperties& mProps_B,
                      const Transform&      X_PF,
                      const Transform&      X_BM,
                      UIndex&               nextUSlot,
                      USquaredIndex&        nextUSqSlot,
                      QIndex&               nextQSlot,
                      QDotHandling          qdotHandling,
                      QuaternionUse         quaternionUse,
                      bool                  isReversed)
      : RigidBodyNode(mProps_B, X_PF, X_BM, qdotHandling, quaternionUse, isReversed)
    {
        // don't call any virtual methods in here!
        uIndex   = nextUSlot;
        uSqIndex = nextUSqSlot;
        qIndex   = nextQSlot;
    }

    void updateSlots(UIndex& nextUSlot, USquaredIndex& nextUSqSlot, QIndex& nextQSlot) {
        // OK to call virtual method here.
        nextUSlot   += getDOF();
        nextUSqSlot += getDOF()*getDOF();
        nextQSlot   += getMaxNQ();
    }

    virtual ~RigidBodyNodeSpec() {}

    // This is the type of the joint transition matrix H, but our definition
    // of H is transposed from Jain's or Schwieters'. That is, what we're 
    // calling H here is what Jain calls H* and Schwieters calls H^T. So
    // our H matrix is 6 x nu, or more usefully it is 2 rows of Vec3's.
    // The first row define how u's contribute to angular velocities; the
    // second defines how u's contribute to linear velocities.
    typedef Mat<2,dof,Vec3> HType;

    // Provide default implementations for setQToFitTransformImpl() and setQToFitVelocityImpl() 
    // which are implemented using the rotational and translational quantity routines. These assume
    // that the rotational and translational coordinates are independent, with rotation handled
    // first and then left alone. If a mobilizer type needs to deal with rotation and
    // translation simultaneously, it should provide a specific implementation for these two routines.
    // *Each* mobilizer must implement setQToFit{Rotation,Translation} and 
    // setUToFit{AngularVelocity,LinearVelocity}; there are no defaults.

    virtual void setQToFitTransformImpl(const SBStateDigest& sbs, const Transform& X_FM, Vector& q) const {
        setQToFitRotationImpl   (sbs,X_FM.R(),q);
        setQToFitTranslationImpl(sbs,X_FM.p(),q);
    }

    virtual void setUToFitVelocityImpl(const SBStateDigest& sbs, const Vector& q, const SpatialVec& V_FM, Vector& u) const {
        setUToFitAngularVelocityImpl(sbs,q,V_FM[0],u);
        setUToFitLinearVelocityImpl (sbs,q,V_FM[1],u);
    }

    // The following routines calculate joint-specific position kinematic
    // quantities. They may assume that *all* position kinematics (not just
    // joint-specific) has been done for the parent, and that the position
    // state variables q are available. Each routine may also assume that the
    // previous routines have been called, in the order below.
    // The routines are structured as operators -- they use the State but
    // do not change anything in it, including the cache. Instead they are
    // passed arguments to write their results into. In practice, these
    // arguments will typically be in the State cache (see below).

    // calcJointSinCosQNorm() and calcAcrossJointTransform() must have
    // been called already before calling these.

    // This mandatory routine calculates the joint transition matrix H_FM, giving
    // the change of velocity induced by the generalized speeds u for this 
    // mobilizer, expressed in the mobilizer's inboard "fixed" frame F (attached to
    // this body's parent). 
    // This method constitutes the *definition* of the generalized speeds for
    // a particular joint.
    // This routine can depend on X_FM having already
    // been calculated and available in the PositionCache but must NOT depend
    // on any quantities involving Ground or other bodies.
    // Note: this calculates the H matrix *as defined*; we might reverse
    // the frames in use. So we call this H_F0M0 while the possibly reversed
    // version is H_FM. Be sure to use X_F0M0 in your calculation for H_F0M0
    // if you need access to the cross-mobilizer transform.
    // CAUTION: our definition of H is transposed from Jain and Schwieters.
    virtual void calcAcrossJointVelocityJacobian(
        const SBStateDigest& sbs,
        HType&               H_F0M0) const=0;

    // This mandatory routine calculates the time derivative taken in F of
    // the matrix H_FM (call it HDot_FM). This is zero if the generalized
    // speeds are all defined in terms of the F frame, which is often the case.
    // This routine can depend on X_FM and H_FM being available already in
    // the state PositionCache, and V_FM being already in the state VelocityCache.
    // However, it must NOT depend on any quantities involving Ground or other bodies.
    // Note: this calculates the HDot matrix *as defined*; we might reverse
    // the frames in use. So we call this HDot_F0M0 while the possibly reversed
    // version is HDot_FM. Be sure to use X_F0M0 and V_F0M0 in your calculation
    // of HDot_F0M0 if you need access to the cross-mobilizer transform and/or velocity.
    // CAUTION: our definition of H is transposed from Jain and Schwieters.
    virtual void calcAcrossJointVelocityJacobianDot(
        const SBStateDigest& sbs,
        HType&               HDot_F0M0) const = 0;

    // We allow a mobilizer to be defined in reverse when that is more
    // convenient. That is, the H matrix can be defined by giving H_F0M0=H_MF and 
    // HDot_F0M0=HDot_MF instead of H_FM and HDot_FM. In that case the 
    // following methods are called instead of the above; the default 
    // implementation postprocesses the output from the above methods, but a 
    // mobilizer can override it if it knows how to get the job done faster.
    virtual void calcReverseMobilizerH_FM(
        const SBStateDigest& sbs,
        HType&               H_FM) const;

    virtual void calcReverseMobilizerHDot_FM(
        const SBStateDigest& sbs,
        HType&               HDot_FM) const;

    // This routine is NOT joint specific, but cannot be called until the across-joint
    // transform X_FM has been calculated and is available in the State cache.
    void calcBodyTransforms(
        const SBPositionCache& pc, 
        Transform&             X_PB, 
        Transform&             X_GB) const 
    {
        const Transform& X_MB = getX_MB();   // fixed
        const Transform& X_PF = getX_PF();   // fixed
        const Transform& X_FM = getX_FM(pc); // just calculated
        const Transform& X_GP = getX_GP(pc); // already calculated

        X_PB = X_PF * X_FM * X_MB;
        X_GB = X_GP * X_PB;
    }

    // Same for all mobilizers. The return matrix here is precisely the 
    // one used by Jain and Schwieters, but transposed.
    void calcParentToChildVelocityJacobianInGround(
        const SBModelVars&     mv,
        const SBPositionCache& pc, 
        HType& H_PB_G) const;

    // Same for all mobilizers. This is the time derivative of 
    // the matrix H_PB_G above, with the derivative taken in the 
    // Ground frame.
    void calcParentToChildVelocityJacobianInGroundDot(
        const SBModelVars&     mv,
        const SBPositionCache& pc, 
        const SBVelocityCache& vc, 
        const SBDynamicsCache& dc, 
        HType& HDot_PB_G) const;


    // Calculate joint-specific kinematic quantities dependent on
    // velocities. This routine may assume that *all* position 
    // kinematics (not just joint-specific) has been done for this node,
    // that all velocity kinematics has been done for the parent, and
    // that the velocity state variables (u) are available. The
    // quanitites that must be computed are:
    //   V_FM   relative velocity of B's M frame in P's F frame, 
    //             expressed in F (note: this is also V_PM_F since
    //             F is fixed on P).
    //   V_PB_G  relative velocity of B in P, expr. in G
    // The code is the same for all joints, although parametrized by ndof.
    void calcJointKinematicsVel(
        const SBPositionCache& pc,
        const Vector&          u,
        SBVelocityCache&       vc) const 
    {
        updV_FM(vc)   = getH_FM(pc) * fromU(u);
        updV_PB_G(vc) = getH(pc)    * fromU(u);
    }

    // Calculate joint-specific dynamics quantities dependent on velocities.
    // This method may assume that *all* position & velocity kinematics
    // (not just joint-specific) has been done for this node, and that
    // dynamics has been done for the parent.
    void calcJointDynamics(
        const SBPositionCache& pc,
        const Vector&          u,
        const SBVelocityCache& vc, 
        SBDynamicsCache&       dc) const 
    {
        updVD_PB_G(dc) = getHDot(dc) * fromU(u);
    }

    // These next two routines are optional, but if you supply one you
    // must supply the other. (That is, ball-containing joints provide
    // both of these routines.)
    virtual void calcQDot(
        const SBStateDigest&   sbs,
        const Vector&          u,
        Vector&                qdot) const
    {
        assert(qdotHandling == QDotIsAlwaysTheSameAsU);
        toQ(qdot) = fromU(u);        // default is qdot=u
    }

    virtual void calcQDotDot(
        const SBStateDigest&   sbs,
        const Vector&          udot, 
        Vector&                qdotdot) const
    {
        assert(qdotHandling == QDotIsAlwaysTheSameAsU);
        toQ(qdotdot) = fromU(udot);  // default is qdotdot=udot
    }

    void realizeModel(SBStateDigest& sbs) const 
    {
    }

    void realizeInstance(SBStateDigest& sbs) const
    {
    }

    void realizeTime(SBStateDigest& sbs) const
    {
    }

    // Set a new configuration and calculate the consequent kinematics.
    // Must call base-to-tip.
    void realizePosition(SBStateDigest& sbs) const 
    {
        const SBModelVars& mv = sbs.getModelVars();
        const SBModelCache& mc = sbs.getModelCache();
        const SBInstanceCache& ic = sbs.getInstanceCache();
        SBPositionCache& pc = sbs.updPositionCache();
        calcJointSinCosQNorm(mv, mc, ic, sbs.getQ(), pc.sq, pc.cq, sbs.updQErr(), pc.qnorm);

        if (isReversed()) {
            Transform X_MF;
            calcAcrossJointTransform(sbs, sbs.getQ(), X_MF);
            updX_FM(pc) = ~X_MF;
        } else 
            calcAcrossJointTransform(sbs, sbs.getQ(), updX_FM(pc));

        calcBodyTransforms(pc, updX_PB(pc), updX_GB(pc));

        // REMINDER: our H matrix definition is transposed from Jain and Schwieters.
        if (isReversed()) calcReverseMobilizerH_FM       (sbs, updH_FM(pc));
        else              calcAcrossJointVelocityJacobian(sbs, updH_FM(pc));

        calcParentToChildVelocityJacobianInGround(mv,pc, updH(pc));
        calcJointIndependentKinematicsPos(pc);
    }

    // Set new velocities for the current configuration, and calculate
    // all the velocity-dependent terms. Must call base-to-tip.
    void realizeVelocity(SBStateDigest& sbs) const 
    {
        const SBPositionCache& pc = sbs.getPositionCache();
        SBVelocityCache& vc = sbs.updVelocityCache();
        calcQDot(sbs, sbs.getU(), sbs.updQDot());
        calcJointKinematicsVel(pc,sbs.getU(),vc);
        calcJointIndependentKinematicsVel(pc,vc);
    }

    void realizeDynamics(SBStateDigest& sbs) const
    {
        // Mobilizer-specific.
        const SBModelVars& mv = sbs.getModelVars();
        const SBPositionCache& pc = sbs.getPositionCache();
        const SBVelocityCache& vc = sbs.getVelocityCache();
        SBDynamicsCache& dc = sbs.updDynamicsCache();

        // REMINDER: our H matrix definition is transposed from Jain and Schwieters.
        if (isReversed()) calcReverseMobilizerHDot_FM       (sbs, updHDot_FM(dc));
        else              calcAcrossJointVelocityJacobianDot(sbs, updHDot_FM(dc));

        calcParentToChildVelocityJacobianInGroundDot(mv,pc,vc, dc, updHDot(dc));
        calcJointDynamics(pc,sbs.getU(),vc,dc);

        // Mobilizer independent.
        calcJointIndependentDynamicsVel(pc,vc,dc);
    }

    void realizeAcceleration(SBStateDigest& sbs) const
    {
    }

    void realizeReport(SBStateDigest& sbs) const
    {
    }

    // This is a dynamics-stage calculation and must be called tip-to-base (inward).
    void calcArticulatedBodyInertiasInward(
        const SBPositionCache& pc,
        SBDynamicsCache&       dc) const;

    // calcJointIndependentDynamicsVel() must be called after ArticulatedBodyInertias.

    // This dynamics-stage calculation is needed for handling constraints. It
    // must be called base-to-tip (outward);
    void calcYOutward(
        const SBPositionCache& pc,
        SBDynamicsCache&       dc) const;

    // These routines give each node a chance to set appropriate defaults in a piece
    // of the state corresponding to a particular stage. Default implementations here
    // assume non-ball joint; override if necessary.
    virtual void setMobilizerDefaultModelValues   (const SBTopologyCache&, SBModelVars&)  const {}
    virtual void setMobilizerDefaultInstanceValues(const SBModelVars&, SBInstanceVars&) const {}
    virtual void setMobilizerDefaultTimeValues    (const SBModelVars&, SBTimeVars&)    const {}

    virtual void setMobilizerDefaultPositionValues(const SBModelVars& s, Vector& q) const 
    {
        toQ(q) = 0.;
    }
    virtual void setMobilizerDefaultVelocityValues(const SBModelVars&, Vector& u) const 
    {
        toU(u) = 0.;
    }
    virtual void setMobilizerDefaultDynamicsValues(const SBModelVars&, SBDynamicsVars&) const {}
    virtual void setMobilizerDefaultAccelerationValues(const SBModelVars&, 
                                                       SBDynamicsVars& v) const {}

    // copyQ and copyU extract this node's values from the supplied
    // q-sized or u-sized array and put them in the corresponding
    // locations in the output variable. Joints which need quaternions should
    // override copyQ to copy the extra q.
    virtual void copyQ(
        const SBModelVars& mv, 
        const Vector&      qIn, 
        Vector&            q) const
    {
        assert(quaternionUse == QuaternionIsNeverUsed);
        toQ(q) = fromQ(qIn);
    }

    // Not virtual -- the number of u's is always the template argument dof.
    void copyU(
        const SBModelVars& mv, 
        const Vector&      uIn, 
        Vector&            u) const
    {
        toU(u) = fromU(uIn);
    }

    int          getDOF()            const {return dof;}
    virtual int  getMaxNQ()          const {
        assert(quaternionUse == QuaternionIsNeverUsed);
        return dof; // maxNQ can be larger than dof if there's a quaternion
    }
    virtual int  getNQInUse(const SBModelVars&) const {
        assert(quaternionUse == QuaternionIsNeverUsed); // method must be overridden otherwise
        return dof; // DOF <= NQ <= maxNQ
    }
    virtual int  getNUInUse(const SBModelVars&) const {
        // Currently NU is always just the Mobilizer's DOFs (the template argument)
        // Later we may want to offer modeling options to lock joints, or perhaps
        // break them.
        return dof;
    }
    virtual bool isUsingQuaternion(const SBStateDigest&, MobilizerQIndex& startOfQuaternion) const {
        assert(quaternionUse == QuaternionIsNeverUsed); // method must be overridden otherwise
        startOfQuaternion.invalidate();
        return false;
    }

    // Most mobilizers do use angles, so we're not going to provide a default implementation
    // of the pure virtual isUsingAngles() method here.

    // State digest should be at Stage::Position.
    virtual void calcLocalQDotFromLocalU(const SBStateDigest&, const Real* u, Real* qdot) const {
        assert(qdotHandling == QDotIsAlwaysTheSameAsU);
        Vec<dof>::updAs(qdot) = Vec<dof>::getAs(u); // default says qdot=u
    }

    // State digest should be at Stage::Velocity.
    virtual void calcLocalQDotDotFromLocalUDot(const SBStateDigest&, const Real* udot, Real* qdotdot) const {
        assert(qdotHandling == QDotIsAlwaysTheSameAsU);
        Vec<dof>::updAs(qdotdot) = Vec<dof>::getAs(udot); // default says qdotdot=udot
    }

    // State digest should be at Stage::Position for calculating Q (the matrix which maps
    // generalized speeds u to coordinate derivatives qdot, qdot=Qu). The default implementation
    // assumes nq==nu and the nqXnu block of Q corresponding to this mobilizer is identity. Then 
    // either operation (regardless of side) just copies nu numbers from in to out.
    //
    // THIS MUST BE OVERRIDDEN by any mobilizer for which nq != nu, or qdot != u.
    virtual void multiplyByN(const SBStateDigest&, bool useEulerAnglesIfPossible, const Real* q,
                                  bool matrixOnRight,  const Real* in, Real* out) const
    {
        assert(qdotHandling == QDotIsAlwaysTheSameAsU);
        Vec<dof>::updAs(out) = Vec<dof>::getAs(in);
    }

    virtual void multiplyByNInv(const SBStateDigest&, bool useEulerAnglesIfPossible, const Real* q,
                                     bool matrixOnRight, const Real* in, Real* out) const
    {
        assert(qdotHandling == QDotIsAlwaysTheSameAsU);
        Vec<dof>::updAs(out) = Vec<dof>::getAs(in);
    }

    // State digest should be at Stage::Velocity for calculating QDot (the matrix which is used in
    // mapping generalized accelerations udot to coordinate 2nd derivatives qdotdot, 
    // qdotdot=Q udot + QDot u. The default implementation assumes nq==nu and the nqXnu block
    // of Q corresponding to this mobilizer is identity, so QDot is an nuXnu block of zeroes.
    // Then either operation (regardless of side) just copies nu zeros to out.
    //
    // THIS MUST BE OVERRIDDEN by any mobilizer for which nq != nu, or qdot != u.
    /* TODO
    virtual void multiplyByNDot(const SBStateDigest&, bool useEulerAnglesIfPossible, 
                                     const Real* q, const Real* u,
                                     bool matrixOnRight, const Real* in, Real* out) const
    {
        assert(qdotHandling == QDotIsAlwaysTheSameAsU);
        Vec<dof>::updAs(out) = 0;
    }
    */

    // No default implementations here for:
    // calcMobilizerTransformFromQ
    // calcMobilizerVelocityFromU
    // calcMobilizerAccelerationFromUDot
    // calcParentToChildTransformFromQ
    // calcParentToChildVelocityFromU
    // calcParentToChildAccelerationFromUDot


    virtual void setVelFromSVel(
        const SBPositionCache& pc, 
        const SBVelocityCache& mc,
        const SpatialVec&      sVel, 
        Vector&               u) const;

    // Return true if any change is made to the q vector.
    virtual bool enforceQuaternionConstraints(
        const SBStateDigest& sbs,
        Vector&            q,
        Vector&            qErrest) const 
    {
        assert(quaternionUse == QuaternionIsNeverUsed);
        return false;
    }

    void convertToEulerAngles(const Vector& inputQ, Vector& outputQ) const {
        // The default implementation just copies Q.  Subclasses may override this.
        assert(quaternionUse == QuaternionIsNeverUsed);
        toQ(outputQ) = fromQ(inputQ);
    }
    
    void convertToQuaternions(const Vector& inputQ, Vector& outputQ) const {
        // The default implementation just copies Q.  Subclasses may override this.
        assert(quaternionUse == QuaternionIsNeverUsed);
        toQ(outputQ) = fromQ(inputQ);
    }

    // Get a column of H_PB_G, which is what Jain calls H* and Schwieters calls H^T.
    const SpatialVec& getHCol(const SBPositionCache& pc, int j) const {
        return getH(pc)(j);
    }

    // Access to body-oriented state and cache entries is the same for all nodes,
    // and joint oriented access is almost the same but parametrized by dof. There is a special
    // case for quaternions because they use an extra state variable, and although we don't
    // have to we make special scalar routines available for 1-dof joints. Note that all State access
    // routines are inline, not virtual, so the cost is just an indirection and an index.
    //
    // TODO: these inner-loop methods probably shouldn't be indexing a Vector, which requires
    // several indirections. Instead, the top-level caller should find the Real* data contained in the
    // Vector and then pass that to the RigidBodyNode routines which will call these ones.

    // General joint-dependent select-my-goodies-from-the-pool routines.
    const Vec<dof>&     fromQ  (const Real* q)   const {return Vec<dof>::getAs(&q[qIndex]);}
    Vec<dof>&           toQ    (      Real* q)   const {return Vec<dof>::updAs(&q[qIndex]);}
    const Vec<dof>&     fromU  (const Real* u)   const {return Vec<dof>::getAs(&u[uIndex]);}
    Vec<dof>&           toU    (      Real* u)   const {return Vec<dof>::updAs(&u[uIndex]);}
    const Mat<dof,dof>& fromUSq(const Real* uSq) const {return Mat<dof,dof>::getAs(&uSq[uSqIndex]);}
    Mat<dof,dof>&       toUSq  (      Real* uSq) const {return Mat<dof,dof>::updAs(&uSq[uSqIndex]);}

    // Same, but specialized for the common case where dof=1 so everything is scalar.
    const Real& from1Q  (const Real* q)   const {return q[qIndex];}
    Real&       to1Q    (      Real* q)   const {return q[qIndex];}
    const Real& from1U  (const Real* u)   const {return u[uIndex];}
    Real&       to1U    (      Real* u)   const {return u[uIndex];}
    const Real& from1USq(const Real* uSq) const {return uSq[uSqIndex];}
    Real&       to1USq  (      Real* uSq) const {return uSq[uSqIndex];}

    // Same, specialized for quaternions. We're assuming that the quaternion is stored
    // in the *first* four coordinates, if there are more than four altogether.
    const Vec4& fromQuat(const Real* q) const {return Vec4::getAs(&q[qIndex]);}
    Vec4&       toQuat  (      Real* q) const {return Vec4::updAs(&q[qIndex]);}

    // Extract a Vec3 from a Q-like or U-like object, beginning at an offset from the qIndex or uIndex.
    const Vec3& fromQVec3(const Real* q, int offs) const {return Vec3::getAs(&q[qIndex+offs]);}
    Vec3&       toQVec3  (      Real* q, int offs) const {return Vec3::updAs(&q[qIndex+offs]);}
    const Vec3& fromUVec3(const Real* u, int offs) const {return Vec3::getAs(&u[uIndex+offs]);}
    Vec3&       toUVec3  (      Real* u, int offs) const {return Vec3::updAs(&u[uIndex+offs]);}

    // These provide an identical interface for when q,u are given as Vectors rather than Reals.

    const Vec<dof>&     fromQ  (const Vector& q)   const {return fromQ(&q[0]);} // convert to array of Real
    Vec<dof>&           toQ    (      Vector& q)   const {return toQ  (&q[0]);}
    const Vec<dof>&     fromU  (const Vector& u)   const {return fromU(&u[0]);}
    Vec<dof>&           toU    (      Vector& u)   const {return toU  (&u[0]);}
    const Mat<dof,dof>& fromUSq(const Vector& uSq) const {return fromUSq(&uSq[0]);}
    Mat<dof,dof>&       toUSq  (      Vector& uSq) const {return toUSq  (&uSq[0]);}

    const Real& from1Q  (const Vector& q)   const {return from1Q  (&q[0]);}
    Real&       to1Q    (      Vector& q)   const {return to1Q    (&q[0]);}
    const Real& from1U  (const Vector& u)   const {return from1U  (&u[0]);}
    Real&       to1U    (      Vector& u)   const {return to1U    (&u[0]);}
    const Real& from1USq(const Vector& uSq) const {return from1USq(&uSq[0]);}
    Real&       to1USq  (      Vector& uSq) const {return to1USq  (&uSq[0]);}

    // Same, specialized for quaternions. We're assuming that the quaternion comes first in the coordinates.
    const Vec4& fromQuat(const Vector& q) const {return fromQuat(&q[0]);}
    Vec4&       toQuat  (      Vector& q) const {return toQuat  (&q[0]);}

    // Extract a Vec3 from a Q-like or U-like object, beginning at an offset from the qIndex or uIndex.
    const Vec3& fromQVec3(const Vector& q, int offs) const {return fromQVec3(&q[0], offs);}
    Vec3&       toQVec3  (      Vector& q, int offs) const {return toQVec3  (&q[0], offs);}
    const Vec3& fromUVec3(const Vector& u, int offs) const {return fromUVec3(&u[0], offs);}
    Vec3&       toUVec3  (      Vector& u, int offs) const {return toUVec3  (&u[0], offs);}

    // Applications of the above extraction routines to particular interesting items in the State. Note
    // that you can't use these for quaternions since they extract "dof" items.

    // Cache entries (cache is mutable in a const State)

        // Position


    // CAUTION: our H definition is transposed from Jain and Schwieters.
    const HType& getH_FM(const SBPositionCache& pc) const
      { return HType::getAs(&pc.storageForH_FM(0,uIndex)); }
    HType&       updH_FM(SBPositionCache& pc) const
      { return HType::updAs(&pc.storageForH_FM(0,uIndex)); }

    // "H" here should really be H_PB_G, that is, cross joint transition
    // matrix relating parent and body frames, but expressed in Ground.
    // CAUTION: our H definition is transposed from Jain and Schwieters.
    const HType& getH(const SBPositionCache& pc) const
      { return HType::getAs(&pc.storageForH(0,uIndex)); }
    HType&       updH(SBPositionCache& pc) const
      { return HType::updAs(&pc.storageForH(0,uIndex)); }

    // These are sines and cosines of angular qs. The rest of the slots are garbage.
    const Vec<dof>&   getSinQ (const SBPositionCache& pc) const {return fromQ (pc.sq);}
    Vec<dof>&         updSinQ (SBPositionCache&       pc) const {return toQ   (pc.sq);}
    const Real&       get1SinQ(const SBPositionCache& pc) const {return from1Q(pc.sq);}
    Real&             upd1SinQ(SBPositionCache&       pc) const {return to1Q  (pc.sq);}

    const Vec<dof>&   getCosQ (const SBPositionCache& pc) const {return fromQ (pc.cq);}
    Vec<dof>&         updCosQ (SBPositionCache&       pc) const {return toQ   (pc.cq);}
    const Real&       get1CosQ(const SBPositionCache& pc) const {return from1Q(pc.cq);}
    Real&             upd1CosQ(SBPositionCache&       pc) const {return to1Q  (pc.cq);}

    // These are normalized quaternions in slots for balls. Everything else is garbage.
    const Vec4&       getQNorm(const SBPositionCache& pc) const {return fromQuat(pc.qnorm);}
    Vec4&             updQNorm(SBPositionCache&       pc) const {return toQuat  (pc.qnorm);}

        // Velocity

        // Dynamics

    // CAUTION: our H definition is transposed from Jain and Schwieters.
    const HType& getHDot_FM(const SBDynamicsCache& dc) const
      { return HType::getAs(&dc.storageForHDot_FM(0,uIndex)); }
    HType&       updHDot_FM(SBDynamicsCache& dc) const
      { return HType::updAs(&dc.storageForHDot_FM(0,uIndex)); }

    // CAUTION: our H definition is transposed from Jain and Schwieters.
    const HType& getHDot(const SBDynamicsCache& dc) const
      { return HType::getAs(&dc.storageForHDot(0,uIndex)); }
    HType&       updHDot(SBDynamicsCache& dc) const
      { return HType::updAs(&dc.storageForHDot(0,uIndex)); }

    const Mat<dof,dof>& getD(const SBDynamicsCache& dc) const {return fromUSq(dc.storageForD);}
    Mat<dof,dof>&       updD(SBDynamicsCache&       dc) const {return toUSq  (dc.storageForD);}

    const Mat<dof,dof>& getDI(const SBDynamicsCache& dc) const {return fromUSq(dc.storageForDI);}
    Mat<dof,dof>&       updDI(SBDynamicsCache&       dc) const {return toUSq  (dc.storageForDI);}

    const Mat<2,dof,Vec3>& getG(const SBDynamicsCache& dc) const
      { return Mat<2,dof,Vec3>::getAs(&dc.storageForG(0,uIndex)); }
    Mat<2,dof,Vec3>&       updG(SBDynamicsCache&       dc) const
      { return Mat<2,dof,Vec3>::updAs(&dc.storageForG(0,uIndex)); }

        // Acceleration

    const Vec<dof>&   getNetHingeForce (const SBAccelerationCache& rc) const {return fromU (rc.netHingeForces);}
    Vec<dof>&         updNetHingeForce (SBAccelerationCache&       rc) const {return toU   (rc.netHingeForces);}
    const Real&       get1NetHingeForce(const SBAccelerationCache& rc) const {return from1U(rc.netHingeForces);}
    Real&             upd1NetHingeForce(SBAccelerationCache&       rc) const {return to1U  (rc.netHingeForces);}


    const Vec<dof>&   getNu (const SBAccelerationCache& rc) const {return fromU (rc.nu);}
    Vec<dof>&         updNu (SBAccelerationCache&       rc) const {return toU   (rc.nu);}
    const Real&       get1Nu(const SBAccelerationCache& rc) const {return from1U(rc.nu);}
    Real&             upd1Nu(SBAccelerationCache&       rc) const {return to1U  (rc.nu);}

    const Vec<dof>&   getEpsilon (const SBAccelerationCache& rc) const {return fromU (rc.epsilon);}
    Vec<dof>&         updEpsilon (SBAccelerationCache&       rc) const {return toU   (rc.epsilon);}
    const Real&       get1Epsilon(const SBAccelerationCache& rc) const {return from1U(rc.epsilon);}
    Real&             upd1Epsilon(SBAccelerationCache&       rc) const {return to1U  (rc.epsilon);}

    void calcZ(
        const SBStateDigest&,
        const Vector&              mobilityForces,
        const Vector_<SpatialVec>& bodyForces) const;

    void calcAccel(
        const SBStateDigest&   sbs,
        Vector&                udot,
        Vector&                qdotdot) const;

    void calcSpatialKinematicsFromInternal(
        const SBPositionCache&      pc,
        const Vector&               v,
        Vector_<SpatialVec>&        Jv) const;

    void calcInternalGradientFromSpatial(
        const SBPositionCache&      pc, 
        Vector_<SpatialVec>&        zTmp,
        const Vector_<SpatialVec>&  X, 
        Vector&                     JX) const;

    void calcEquivalentJointForces(
        const SBPositionCache&      pc,
        const SBDynamicsCache&      dc,
        const Vector_<SpatialVec>&  bodyForces,
        Vector_<SpatialVec>&        allZ,
        Vector&                     jointForces) const;

    void calcUDotPass1Inward(
        const SBPositionCache&      pc,
        const SBDynamicsCache&      dc,
        const Vector&               jointForces,
        const Vector_<SpatialVec>&  bodyForces,
        Vector_<SpatialVec>&        allZ,
        Vector_<SpatialVec>&        allGepsilon,
        Vector&                     allEpsilon) const;

    void calcUDotPass2Outward(
        const SBPositionCache&      pc,
        const SBDynamicsCache&      dc,
        const Vector&               epsilonTmp,
        Vector_<SpatialVec>&        allA_GB,
        Vector&                     allUDot) const;

    void calcMInverseFPass1Inward(
        const SBPositionCache&      pc,
        const SBDynamicsCache&      dc,
        const Vector&               f,
        Vector_<SpatialVec>&        allZ,
        Vector_<SpatialVec>&        allGepsilon,
        Vector&                     allEpsilon) const;

    void calcMInverseFPass2Outward(
        const SBPositionCache&      pc,
        const SBDynamicsCache&      dc,
        const Vector&               epsilonTmp,
        Vector_<SpatialVec>&        allA_GB,
        Vector&                     allUDot) const;

	void calcMAPass1Outward(
		const SBPositionCache& pc,
		const Vector&          allUDot,
		Vector_<SpatialVec>&   allA_GB) const;
	void calcMAPass2Inward(
		const SBPositionCache& pc,
		const Vector_<SpatialVec>& allA_GB,
		Vector_<SpatialVec>&       allFTmp,
		Vector&                    allTau) const;

};

#endif // SimTK_SIMBODY_RIGID_BODY_NODE_SPEC_H_

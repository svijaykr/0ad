/* Copyright (C) 2010 Wildfire Games.
 * This file is part of 0 A.D.
 *
 * 0 A.D. is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * 0 A.D. is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with 0 A.D.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "precompiled.h"

#include "simulation2/system/Component.h"
#include "ICmpUnitMotion.h"

#include "ICmpPosition.h"
#include "ICmpPathfinder.h"
#include "simulation2/MessageTypes.h"

class CCmpUnitMotion : public ICmpUnitMotion
{
public:
	static void ClassInit(CComponentManager& componentManager)
	{
		componentManager.SubscribeToMessageType(MT_Update);
	}

	DEFAULT_COMPONENT_ALLOCATOR(UnitMotion)

	const CSimContext* m_Context;

	// Template state:
	CFixed_23_8 m_Speed; // in units per second

	// Dynamic state:
	bool m_HasTarget;
	ICmpPathfinder::Path m_Path;
	entity_pos_t m_TargetX, m_TargetZ; // these values contain undefined junk if !HasTarget

	virtual void Init(const CSimContext& context, const CParamNode& UNUSED(paramNode))
	{
		m_Context = &context;
		m_Speed = CFixed_23_8::FromInt(4);
		m_HasTarget = false;
	}

	virtual void Deinit(const CSimContext& UNUSED(context))
	{
	}

	virtual void Serialize(ISerializer& serialize)
	{
		serialize.Bool("has target", m_HasTarget);
		if (m_HasTarget)
		{
			// TODO: m_Path
			serialize.NumberFixed_Unbounded("target x", m_TargetX);
			serialize.NumberFixed_Unbounded("target z", m_TargetZ);
		}
	}

	virtual void Deserialize(const CSimContext& context, const CParamNode& paramNode, IDeserializer& deserialize)
	{
		Init(context, paramNode);

		deserialize.Bool(m_HasTarget);
		if (m_HasTarget)
		{
			deserialize.NumberFixed_Unbounded(m_TargetX);
			deserialize.NumberFixed_Unbounded(m_TargetZ);
		}
	}

	virtual void HandleMessage(const CSimContext& context, const CMessage& msg, bool UNUSED(global))
	{
		switch (msg.GetType())
		{
		case MT_Update:
		{
			CFixed_23_8 dt = static_cast<const CMessageUpdate&> (msg).turnLength;
			Move(context, dt);
			break;
		}
		}
	}

	virtual void MoveToPoint(entity_pos_t x, entity_pos_t z)
	{
		CmpPtr<ICmpPathfinder> cmpPathfinder (*m_Context, SYSTEM_ENTITY);
		if (cmpPathfinder.null())
			return;

		CmpPtr<ICmpPosition> cmpPosition(*m_Context, GetEntityId());
		if (cmpPosition.null())
			return;

		CFixedVector3D pos = cmpPosition->GetPosition();

		m_Path.m_Waypoints.clear();

		u32 cost;
		entity_pos_t r = entity_pos_t::FromInt(0); // TODO: should get this from the entity's size
		if (cmpPathfinder->CanMoveStraight(pos.X, pos.Z, x, z, r, cost))
		{
			m_TargetX = x;
			m_TargetZ = z;
			m_HasTarget = true;
		}
		else
		{
			cmpPathfinder->SetDebugPath(pos.X, pos.Z, x, z);
			cmpPathfinder->ComputePath(pos.X, pos.Z, x, z, m_Path);
			if (!m_Path.m_Waypoints.empty())
				PickNextWaypoint(pos);
		}
	}

	void Move(const CSimContext& context, CFixed_23_8 dt);

	void PickNextWaypoint(const CFixedVector3D& pos);
};

REGISTER_COMPONENT_TYPE(UnitMotion)


void CCmpUnitMotion::Move(const CSimContext& context, CFixed_23_8 dt)
{
	if (!m_HasTarget)
		return;

	CmpPtr<ICmpPosition> cmpPosition(context, GetEntityId());
	if (cmpPosition.null())
		return;

	CFixed_23_8 maxdist = m_Speed.Multiply(dt);

	CFixedVector3D pos = cmpPosition->GetPosition();
	pos.Y = CFixed_23_8::FromInt(0); // remove Y so it doesn't influence our distance calculations

	// We want to move (at most) m_Speed*dt units from pos towards the next waypoint

	while (dt > CFixed_23_8::FromInt(0))
	{
		CFixedVector3D target(m_TargetX, CFixed_23_8::FromInt(0), m_TargetZ);
		CFixedVector3D offset = target - pos;

		// Face towards the target
		entity_angle_t angle = atan2_approx(offset.X, offset.Z);
		cmpPosition->SetYRotation(angle);

		// If it's close, we can move there directly
		if (offset.Length() <= maxdist)
		{
			// If we've reached the last waypoint, stop
			if (m_Path.m_Waypoints.empty())
			{
				cmpPosition->MoveTo(target.X, target.Z);
				m_HasTarget = false;
				return;
			}

			// Otherwise, spend the rest of the time heading towards the next waypoint
			dt = dt - dt.Multiply(offset.Length() / maxdist);
			pos = target;
			PickNextWaypoint(pos);
			continue;
		}
		else
		{
			// Not close enough, so just move in the right direction
			offset.Normalize(maxdist);
			pos += offset;
			cmpPosition->MoveTo(pos.X, pos.Z);
			return;
		}
	}
}

void CCmpUnitMotion::PickNextWaypoint(const CFixedVector3D& pos)
{
	// We can always pick the immediate next waypoint
	debug_assert(!m_Path.m_Waypoints.empty());
	m_TargetX = m_Path.m_Waypoints.back().x;
	m_TargetZ = m_Path.m_Waypoints.back().z;
	m_Path.m_Waypoints.pop_back();
	m_HasTarget = true;

	// To smooth the motion and avoid grid-constrained motion, we could try picking some
	// subsequent waypoints instead, if we can reach them without hitting any obstacles

	CmpPtr<ICmpPathfinder> cmpPathfinder (*m_Context, SYSTEM_ENTITY);
	if (cmpPathfinder.null())
		return;

	for (size_t i = 0; i < 3 && !m_Path.m_Waypoints.empty(); ++i)
	{
		u32 cost;
		entity_pos_t r = entity_pos_t::FromInt(0); // TODO: should get this from the entity's size
		if (!cmpPathfinder->CanMoveStraight(pos.X, pos.Z, m_Path.m_Waypoints.back().x, m_Path.m_Waypoints.back().z, r, cost))
			break;
		m_TargetX = m_Path.m_Waypoints.back().x;
		m_TargetZ = m_Path.m_Waypoints.back().z;
		m_Path.m_Waypoints.pop_back();
	}
}

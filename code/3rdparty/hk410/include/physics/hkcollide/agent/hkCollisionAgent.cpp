/* 
 * 
 * Confidential Information of Telekinesys Research Limited (t/a Havok).  Not for disclosure or distribution without Havok's
 * prior written consent.This software contains code, techniques and know-how which is confidential and proprietary to Havok.
 * Level 2 and Level 3 source code contains trade secrets of Havok. Havok Software (C) Copyright 1999-2006 Telekinesys Research Limited t/a Havok. All Rights Reserved. Use of this software is subject to the terms of an end user license agreement.
 * 
 */

#include <hkcollide/hkCollide.h>

#include <hkcollide/agent/hkCollisionAgent.h>


void hkCollisionAgent::invalidateTim(hkCollisionInput& input)
{
}

void hkCollisionAgent::warpTime(hkTime oldTime, hkTime newTime, hkCollisionInput& input)
{
}


void hkCollisionAgent::removePoint( hkContactPointId idToRemove )
{
	HK_ASSERT2(0xf021f734, 0, "Internal Warning: Not implemented");
}

void hkCollisionAgent::commitPotential( hkContactPointId newId )
{
	HK_ASSERT2(0xf021f735, 0, "Internal Warning: Not implemented");
}

void hkCollisionAgent::createZombie( hkContactPointId idTobecomeZombie )
{
	HK_ASSERT2(0xf021f736, 0, "Internal Warning: Not implemented");
}

/*
* Havok SDK - CLIENT RELEASE, BUILD(#20060902)
*
* Confidential Information of Havok.  (C) Copyright 1999-2006 
* Telekinesys Research Limited t/a Havok. All Rights Reserved. The Havok
* Logo, and the Havok buzzsaw logo are trademarks of Havok.  Title, ownership
* rights, and intellectual property rights in the Havok software remain in
* Havok and/or its suppliers.
*
* Use of this software for evaluation purposes is subject to and indicates 
* acceptance of the End User licence Agreement for this product. A copy of 
* the license is included with this software and is also available from salesteam@havok.com.
*
*/

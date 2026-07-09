

#include "extension.h"
#include "ihandleentity.h"

#include "tier1/strtools.h"


// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"
#include "vtable_hook_helper.h"



CollisionHook g_CollisionHook;
SMEXT_LINK( &g_CollisionHook );

IGameConfig *g_pGameConf = NULL;
IPhysics *g_pPhysics = NULL;

IForward *g_pCollisionFwd = NULL;
IForward *g_pPassFwd = NULL;

CVTableHook *g_SetCollisionSolverHook, *g_ShouldCollideHook;

bool gPassServerEntityFilterDetoured;

KHook::Return<bool> PassServerEntityFilterFunc(const IHandleEntity *pTouch, const IHandleEntity *pPass)
{
	if ( g_pPassFwd->GetFunctionCount() == 0 )
		return { KHook::Action::Ignore, *(bool*)KHook::GetCurrentValuePtr() };

	if ( pTouch == pPass )
		return { KHook::Action::Ignore, *(bool*)KHook::GetCurrentValuePtr() }; // self checks aren't interesting

	if ( !pTouch || !pPass )
		return { KHook::Action::Ignore, *(bool*)KHook::GetCurrentValuePtr() }; // need two valid entities

	CBaseEntity *pEnt1 = const_cast<CBaseEntity *>( UTIL_EntityFromEntityHandle( pTouch ) );
	CBaseEntity *pEnt2 = const_cast<CBaseEntity *>( UTIL_EntityFromEntityHandle( pPass ) );

	if ( !pEnt1 || !pEnt2 )
		return { KHook::Action::Ignore, *(bool*)KHook::GetCurrentValuePtr() }; // we need both entities

	cell_t ent1 = gamehelpers->EntityToBCompatRef( pEnt1 );
	cell_t ent2 = gamehelpers->EntityToBCompatRef( pEnt2 );

	// todo: do we want to fill result with with the game's result? perhaps the forward path is more performant...
	cell_t result = 0;
	g_pPassFwd->PushCell( ent1 );
	g_pPassFwd->PushCell( ent2 );
	g_pPassFwd->PushCellByRef( &result );

	cell_t retValue = 0;
	g_pPassFwd->Execute( &retValue );

	if ( retValue > Pl_Continue )
	{
		// plugin wants to change the result
		return { KHook::Action::Override, result == 1 };
	}
	
	// otherwise, game decides
	return { KHook::Action::Ignore, *(bool*)KHook::GetCurrentValuePtr() };
}

KHook::Function<bool, const IHandleEntity*, const IHandleEntity*> g_PassServerEntityFilterFuncDetour(nullptr, PassServerEntityFilterFunc);

CollisionHook::CollisionHook() : m_CreateEnvironment(&IPhysics::CreateEnvironment, this, nullptr, &CollisionHook::CreateEnvironment), m_SetCollisionSolver(&IPhysicsEnvironment::SetCollisionSolver, this, nullptr, &CollisionHook::SetCollisionSolver), m_VPhysics_ShouldCollide(&IPhysicsCollisionSolver::ShouldCollide, this, &CollisionHook::VPhysics_ShouldCollide, nullptr)
{
}


bool CollisionHook::SDK_OnLoad( char *error, size_t maxlength, bool late )
{
	void *pPassServerEntityFilterAddr = NULL;

	char szConfError[ 256 ] = "";
	if ( !gameconfs->LoadGameConfigFile( "collisionhook", &g_pGameConf, szConfError, sizeof( szConfError ) ) )
	{
		V_snprintf( error, maxlength, "Could not read collisionhook gamedata: %s", szConfError );
		return false;
	}

	if (!g_pGameConf->GetMemSig("PassServerEntityFilter", &pPassServerEntityFilterAddr) || pPassServerEntityFilterAddr == nullptr) {
		g_pSM->LogError(myself, "Failed to retrieve PassServerEntityFilter.");
		return false;
	}

	g_PassServerEntityFilterFuncDetour.Configure(reinterpret_cast<bool (*)(const IHandleEntity*, const IHandleEntity*)>(pPassServerEntityFilterAddr));
	gPassServerEntityFilterDetoured = true;


	g_pCollisionFwd = forwards->CreateForward( "CH_ShouldCollide", ET_Hook, 3, NULL, Param_Cell, Param_Cell, Param_CellByRef );
	g_pPassFwd = forwards->CreateForward( "CH_PassFilter", ET_Hook, 3, NULL, Param_Cell, Param_Cell, Param_CellByRef );
	
	sharesys->RegisterLibrary( myself, "collisionhook" );

	return true;
}

void CollisionHook::SDK_OnUnload()
{
	forwards->ReleaseForward( g_pCollisionFwd );
	forwards->ReleaseForward( g_pPassFwd );

	gameconfs->CloseGameConfigFile( g_pGameConf );

	if ( gPassServerEntityFilterDetoured )
	{
		g_PassServerEntityFilterFuncDetour.~Function();
		gPassServerEntityFilterDetoured = false;
	}
}

bool CollisionHook::SDK_OnMetamodLoad( ISmmAPI *ismm, char *error, size_t maxlen, bool late )
{
	GET_V_IFACE_CURRENT( GetPhysicsFactory, g_pPhysics, IPhysics, VPHYSICS_INTERFACE_VERSION );


	m_CreateEnvironment.Add(g_pPhysics);

	return true;
}

bool CollisionHook::SDK_OnMetamodUnload(char *error, size_t maxlength)
{
	m_CreateEnvironment.Remove(g_pPhysics);

	delete g_SetCollisionSolverHook;
	delete g_ShouldCollideHook;

	g_pPhysics = NULL;

	return true;
}


KHook::Return<IPhysicsEnvironment*> CollisionHook::CreateEnvironment(IPhysics *pPhysics)
{
	// in order to hook IPhysicsCollisionSolver::ShouldCollide, we need to know when a solver is installed
	// in order to hook any installed solvers, we need to hook any created physics environments

	IPhysicsEnvironment *pEnvironment = *(IPhysicsEnvironment **)KHook::GetCurrentValuePtr();

	if ( !pEnvironment )
		return { KHook::Action::Supersede, pEnvironment }; // just in case

	void** vtable = *(void***)pEnvironment;
	auto func = KHook::GetVtableFunction(pEnvironment, &IPhysicsEnvironment::SetCollisionSolver);


	// Hook globally so we know when any solver is installed
	g_ShouldCollideHook = new CVTableHook(vtable,
		new KHook::Member<IPhysicsEnvironment, void, IPhysicsCollisionSolver*>(
			func,
			this, nullptr, &CollisionHook::SetCollisionSolver
		));
	
	m_CreateEnvironment.Remove(g_pPhysics);
	return { KHook::Action::Supersede, pEnvironment };
}

KHook::Return<void> CollisionHook::SetCollisionSolver( IPhysicsEnvironment *pEnvironment, IPhysicsCollisionSolver *pSolver )
{
	if ( !pSolver )
		return { KHook::Action::Ignore }; // this shouldn't happen, but knowing valve...

	void** vtable = *(void***)pSolver;
	auto func = KHook::GetVtableFunction(pSolver, &IPhysicsCollisionSolver::ShouldCollide);

	// The game installed a solver, globally hook ShouldCollide
	#if SOURCE_ENGINE == SE_LEFT4DEAD2
	g_ShouldCollideHook = new CVTableHook(vtable,
		new KHook::Member<IPhysicsCollisionSolver, int, IPhysicsObject*, IPhysicsObject*, void*, void*, const PhysicsCollisionRulesCache_t &, const PhysicsCollisionRulesCache_t &>(
			func,
			this, &CollisionHook::VPhysics_ShouldCollide, nullptr
		));
	#else
	g_ShouldCollideHook = new CVTableHook(vtable,
		new KHook::Member<IPhysicsCollisionSolver, int, IPhysicsObject*, IPhysicsObject*, void*, void*>(
			func,
			this, &CollisionHook::VPhysics_ShouldCollide, nullptr
		));
	#endif
	delete g_SetCollisionSolverHook; // No longer needed

	return { KHook::Action::Ignore };
}

#if SOURCE_ENGINE == SE_LEFT4DEAD2
KHook::Return<int> CollisionHook::VPhysics_ShouldCollide( IPhysicsCollisionSolver* pSolver, IPhysicsObject *pObj1, IPhysicsObject *pObj2, void *pGameData1, void *pGameData2, const PhysicsCollisionRulesCache_t &objCache1, const PhysicsCollisionRulesCache_t &obhCache2 )
#else
KHook::Return<int> CollisionHook::VPhysics_ShouldCollide( IPhysicsCollisionSolver* pSolver, IPhysicsObject *pObj1, IPhysicsObject *pObj2, void *pGameData1, void *pGameData2 )
#endif
{
	if ( g_pCollisionFwd->GetFunctionCount() == 0 )
		return { KHook::Action::Ignore, 1}; // no plugins are interested, let the game decide

	if ( pObj1 == pObj2 )
		return { KHook::Action::Ignore, 1}; // self collisions aren't interesting

	CBaseEntity *pEnt1 = reinterpret_cast<CBaseEntity *>( pGameData1 );
	CBaseEntity *pEnt2 = reinterpret_cast<CBaseEntity *>( pGameData2 );

	if ( !pEnt1 || !pEnt2 )
		return { KHook::Action::Ignore, 1}; // we need two entities

	cell_t ent1 = gamehelpers->EntityToBCompatRef( pEnt1 );
	cell_t ent2 = gamehelpers->EntityToBCompatRef( pEnt2 );

	// todo: do we want to fill result with with the game's result? perhaps the forward path is more performant...
	cell_t result = 0;
	g_pCollisionFwd->PushCell( ent1 );
	g_pCollisionFwd->PushCell( ent2 );
	g_pCollisionFwd->PushCellByRef( &result );

	cell_t retValue = 0;
	g_pCollisionFwd->Execute( &retValue );

	if ( retValue > Pl_Continue )
	{
		// plugin wants to change the result
		return { KHook::Action::Supersede, result == 1 };
	}

	// otherwise, game decides
	return { KHook::Action::Ignore, 0 };
}

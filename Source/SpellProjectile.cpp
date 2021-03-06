
#include "StdAfx.h"
#include "SpellProjectile.h"
#include "World.h"
#include "Monster.h"
#include "CombatFormulas.h"
#include "Player.h"
#include "fastrand.h"

const float MAX_SPELL_PROJECTILE_LIFETIME = 30.0f;

CSpellProjectile::CSpellProjectile(const SpellCastData &scd, DWORD target_id)//, unsigned int damage)
{
	m_CachedSpellCastData = scd;
	m_SourceID = scd.source_id;
	m_TargetID = target_id;
	//m_Damage = damage;

	m_fSpawnTime = Timer::cur_time;
	m_fDestroyTime = FLT_MAX;

	m_fFriction = 1.0f;
	m_fElasticity = 0.0f;

	m_DefaultScript = 0x5A;
	m_DefaultScriptIntensity = 1.0f;

	m_fEffectMod = max(0, min(1.0, ((scd.power_level_of_power_component - 1.0) / 7.0)));

	SetItemType(ITEM_TYPE::TYPE_SELF);
	
	m_Qualities.SetBool(STUCK_BOOL, TRUE);
	m_Qualities.SetBool(ATTACKABLE_BOOL, TRUE);
	m_Qualities.SetBool(UI_HIDDEN_BOOL, TRUE);

	SetInitialPhysicsState(INELASTIC_PS | SCRIPTED_COLLISION_PS | REPORT_COLLISIONS_PS | MISSILE_PS | LIGHTING_ON_PS | PATHCLIPPED_PS | ALIGNPATH_PS);
}


CSpellProjectile::~CSpellProjectile()
{
}

void CSpellProjectile::Tick()
{
	if (!m_bDestroyMe)
	{
		if (!InValidCell() || (m_fDestroyTime <= Timer::cur_time))
		{
			MarkForDestroy();
		}
		// not destroyed yet and distance/time exceeded
		else if (m_fDestroyTime > Timer::cur_time + 10 && (m_Position.distance(m_CachedSpellCastData.initial_cast_position) > m_CachedSpellCastData.max_range || (m_fSpawnTime + MAX_SPELL_PROJECTILE_LIFETIME - 1) <= Timer::cur_time))
		{
			HandleExplode();
			m_fDestroyTime = Timer::cur_time + 1;
		}
	}
}

void CSpellProjectile::PostSpawn()
{
	CWeenieObject::PostSpawn();

	EmitEffect(PS_Launch, m_fEffectMod);
	m_fSpawnTime = Timer::cur_time;
}

void CSpellProjectile::HandleExplode()
{
	EmitEffect(PS_Explode, m_fEffectMod);

	// INELASTIC_PS | SCRIPTED_COLLISION_PS | REPORT_COLLISIONS_PS | MISSILE_PS | LIGHTING_ON_PS | PATHCLIPPED_PS | ALIGNPATH_PS
	set_state(/*0x128374*/
		INELASTIC_PS | SCRIPTED_COLLISION_PS | MISSILE_PS | PATHCLIPPED_PS | ALIGNPATH_PS | ETHEREAL_PS | IGNORE_COLLISIONS_PS | NODRAW_PS | CLOAKED_PS, TRUE);

	// no more REPORT_COLLISIONS_PS and no more LIGHTING_ON_PS
}

BOOL CSpellProjectile::DoCollision(const class EnvCollisionProfile &prof)
{
	Movement_UpdatePos();
	HandleExplode();
	m_fDestroyTime = Timer::cur_time + 1.0;

	return CWeenieObject::DoCollision(prof);
}

BOOL CSpellProjectile::DoCollision(const class AtkCollisionProfile &prof)
{
	CWeenieObject *pHit = g_pWorld->FindWithinPVS(this, prof.id);
	if (pHit && (!m_TargetID || m_TargetID == pHit->GetID()) && (pHit->GetID() != m_SourceID))
	{
		CWeenieObject *pSource = NULL;
		if (m_SourceID)
			pSource = g_pWorld->FindObject(m_SourceID);

		if (!pHit->ImmuneToDamage(pSource))
		{
			Movement_UpdatePos();
			// EmitEffect(5, 0.8f);
			HandleExplode();
			m_fDestroyTime = Timer::cur_time + 1.0;
			
			if (pSource && pHit && pSource->AsPlayer() && pHit->AsPlayer())
			{
				pSource->AsPlayer()->UpdatePKActivity();
				pHit->AsPlayer()->UpdatePKActivity();
			}

			// try to resist
			bool bResisted = false;

			if (m_CachedSpellCastData.spell->_bitfield & Resistable_SpellIndex)
			{
				if (pSource != pHit)
				{
					if (pHit->TryMagicResist(m_CachedSpellCastData.current_skill))
					{
						pHit->EmitSound(Sound_ResistSpell, 1.0f, false);

						if (pSource)
						{
							pHit->SendText(csprintf("You resist the spell cast by %s", pSource->GetName().c_str()), LTT_MAGIC);
							pSource->SendText(csprintf("%s resists your spell", pHit->GetName().c_str()), LTT_MAGIC);
							pHit->OnResistSpell(pSource);
						}
						bResisted = true;
					}
				}
			}

			if (!bResisted)
			{
				CWeenieObject *wand = g_pWorld->FindObject(m_CachedSpellCastData.wand_id);

				int preVarianceDamage;
				double baseDamage;
				if (isLifeProjectile)
				{
					preVarianceDamage = selfDrainedAmount;
					baseDamage = selfDrainedAmount * selfDrainedDamageRatio;
				}
				else
				{
					ProjectileSpellEx *meta = (ProjectileSpellEx *)m_CachedSpellCastData.spellEx->_meta_spell._spell;
					double minDamage = (double)meta->_baseIntensity;
					double maxDamage = (double)minDamage + meta->_variance;

					preVarianceDamage = maxDamage;
					baseDamage = FastRNG.NextDouble(minDamage, maxDamage);

					if (wand)
					{
						double elementalDamageMod = wand->InqDamageType() == InqDamageType() ? wand->InqFloatQuality(ELEMENTAL_DAMAGE_MOD_FLOAT, 1.0) : 1.0;
						if (pSource->AsPlayer() && pHit->AsPlayer()) //pvp
							elementalDamageMod = ((elementalDamageMod - 1.0) / 2.0) + 1.0;
						baseDamage *= elementalDamageMod;
					}
				}

				DamageEventData dmgEvent;
				dmgEvent.source = pSource;
				dmgEvent.target = pHit;
				dmgEvent.weapon = wand;
				dmgEvent.damage_form = DF_MAGIC;
				dmgEvent.damage_type = InqDamageType();
				dmgEvent.hit_quadrant = DAMAGE_QUADRANT::DQ_UNDEF; //should spells have hit quadrants?
				dmgEvent.attackSkill = m_CachedSpellCastData.spell->InqSkillForSpell();
				dmgEvent.attackSkillLevel = m_CachedSpellCastData.current_skill;
				dmgEvent.preVarianceDamage = preVarianceDamage;
				dmgEvent.baseDamage = baseDamage;

				dmgEvent.isProjectileSpell = true;
				dmgEvent.spell_name = m_CachedSpellCastData.spell->_name;

				CalculateDamage(&dmgEvent, &m_CachedSpellCastData);

				pHit->TryToDealDamage(dmgEvent);

				if (pSource && pSource->AsPlayer())
				{
					// update the target's health on the casting player asap
					((CPlayerWeenie*)pSource)->RefreshTargetHealth();
				}

			}
		}
	}

	return CWeenieObject::DoCollision(prof);
}

BOOL CSpellProjectile::DoCollision(const class ObjCollisionProfile &prof)
{
	Movement_UpdatePos();
	// EmitEffect(5, 0.8f);
	HandleExplode();
	m_fDestroyTime = Timer::cur_time + 1.0;

	return CWeenieObject::DoCollision(prof);
}

void CSpellProjectile::DoCollisionEnd(DWORD object_id)
{
	CWeenieObject::DoCollisionEnd(object_id);
}


void CSpellProjectile::makeLifeProjectile(int iSelfDrainedAmount, float fSelfDrainedDamageRatio)
{
	isLifeProjectile = true;
	selfDrainedAmount = iSelfDrainedAmount;
	selfDrainedDamageRatio = fSelfDrainedDamageRatio;
}


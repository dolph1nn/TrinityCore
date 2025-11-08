/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * He should call trash(from the room with Embalming Slime up to frogger(not sure
  if trash before Embalming Slime too or not)) after aggro
 */

#include "ScriptMgr.h"
#include "InstanceScript.h"
#include "naxxramas.h"
#include "ScriptedCreature.h"

enum PatchwerkTexts
{
    SAY_AGGRO                   = 0,
    SAY_SLAY                    = 1,
    SAY_DEATH                   = 2,
    EMOTE_BERSERK               = 3,
    EMOTE_FRENZY                = 4
};

enum PatchwerkSpells
{
    SPELL_HATEFUL_STRIKE_PRIMER = 28307,
    SPELL_HATEFUL_STRIKE        = 28308,
    SPELL_FRENZY                = 28131,
    SPELL_BERSERK               = 26662,
    SPELL_SLIME_BOLT            = 32309
};

enum PatchwerkEvents
{
    EVENT_BERSERK = 1,
    EVENT_HATEFUL,
    EVENT_SLIME
};

enum Misc
{
    ACHIEV_MAKE_QUICK_WERK_OF_HIM_STARTING_EVENT  = 10286
};

enum HatefulThreatAmounts
{
    HATEFUL_THREAT_AMT  = 1000,
};

struct boss_patchwerk : public BossAI
{
    boss_patchwerk(Creature* creature) : BossAI(creature, BOSS_PATCHWERK), _enraged(false) { }

    void Reset() override
    {
        _Reset();
        _enraged = false;

        instance->DoStopTimedAchievement(ACHIEVEMENT_TIMED_TYPE_EVENT, ACHIEV_MAKE_QUICK_WERK_OF_HIM_STARTING_EVENT);
    }

    void KilledUnit(Unit* /*Victim*/) override
    {
        // 20s cd, not random
        if (!(rand32() % 5))
            Talk(SAY_SLAY);
    }

    void JustDied(Unit* /*killer*/) override
    {
        _JustDied();
        Talk(SAY_DEATH);
    }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);
        Talk(SAY_AGGRO);
        events.ScheduleEvent(EVENT_HATEFUL, 3600ms);
        events.ScheduleEvent(EVENT_BERSERK, 6min);

        instance->DoStartTimedAchievement(ACHIEVEMENT_TIMED_TYPE_EVENT, ACHIEV_MAKE_QUICK_WERK_OF_HIM_STARTING_EVENT);
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
            return;

        events.Update(diff);

        while (uint32 eventId = events.ExecuteEvent())
        {
            switch (eventId)
            {
                // Move everything to SPELL_HATEFUL_STRIKE_PRIMER spell script
                case EVENT_HATEFUL:
                {
                    // Hateful Strike targets the highest non-MT threat in melee range on 10man
                    // and the higher HP target out of the top non-MT threats in melee range on 25man
                    ThreatManager& mgr = me->GetThreatManager();
                    Unit* currentVictim = mgr.GetCurrentVictim();
                    auto list = mgr.GetModifiableThreatList();
                    auto it = list.begin(), end = list.end();
                    if (it == end)
                    {
                        EnterEvadeMode(EVADE_REASON_NO_HOSTILES);
                        return;
                    }

                    // Build list of valid melee range targets (excluding MT)
                    // For 10-man: Find the highest non-MT threat that IS in melee range
                    // For 25-man: Find the top 2 non-MT threats that ARE in melee range
                    std::vector<ThreatReference*> meleeThreats;
                    uint32 maxThreatsToCheck = 10; // Check up to 10 non-MT threats to find valid melee targets
                    uint32 nonMTThreatsChecked = 0;
                    uint32 meleeTargetsNeeded = Is25ManRaid() ? 2 : 1; // Need 2 melee targets for 25man, 1 for 10man

                    for (auto threatIt = list.begin(); threatIt != end && nonMTThreatsChecked < maxThreatsToCheck && meleeThreats.size() < meleeTargetsNeeded; ++threatIt)
                    {
                        ThreatReference* threatRef = *threatIt;
                        if (!threatRef->IsAvailable())
                            continue;

                        Unit* threatTarget = threatRef->GetVictim();
                        if (!threatTarget || threatTarget == currentVictim)
                            continue;

                        // Count this as a checked non-MT threat
                        nonMTThreatsChecked++;

                        // Only consider targets in melee range
                        if (me->IsWithinMeleeRange(threatTarget))
                        {
                            meleeThreats.push_back(threatRef);
                            // For 10-man: Stop once we find the first (highest threat) melee target
                            // For 25-man: Continue until we find 2 melee targets
                        }
                    }

                    // Also check if MT is in melee range
                    bool mtInMelee = me->IsWithinMeleeRange(currentVictim);

                    Unit* pHatefulTarget = nullptr;

                    if (Is25ManRaid())
                    {
                        // 25-man logic: Take top threats, filter to melee, then pick highest HP
                        // If > 1 target in melee (including MT), ignore MT and pick highest HP from non-MT
                        // If exactly 1 target in melee, that's the MT, hit MT
                        uint32 totalMeleeTargets = meleeThreats.size() + (mtInMelee ? 1 : 0);
                        
                        if (totalMeleeTargets > 1)
                        {
                            // Multiple targets in melee, ignore MT and pick highest HP from non-MT targets
                            if (meleeThreats.empty())
                            {
                                // This shouldn't happen if totalMeleeTargets > 1, but fallback to MT
                                pHatefulTarget = mtInMelee ? currentVictim : nullptr;
                            }
                            else
                            {
                                // Pick highest HP from non-MT melee targets
                                Unit* highestHPTarget = nullptr;
                                uint32 highestHP = 0;
                                for (ThreatReference* threatRef : meleeThreats)
                                {
                                    Unit* target = threatRef->GetVictim();
                                    uint32 targetHP = target->GetHealth();
                                    if (targetHP > highestHP)
                                    {
                                        highestHP = targetHP;
                                        highestHPTarget = target;
                                    }
                                }
                                pHatefulTarget = highestHPTarget;
                            }
                        }
                        else if (totalMeleeTargets == 1)
                        {
                            // Exactly one target in melee, that's the MT
                            pHatefulTarget = mtInMelee ? currentVictim : nullptr;
                        }
                        else
                        {
                            // No targets in melee range
                            pHatefulTarget = nullptr;
                        }
                    }
                    else
                    {
                        // 10-man logic: Highest non-MT threat in melee, or MT if no one else in melee
                        if (meleeThreats.empty())
                        {
                            // No non-MT targets in melee, hit MT if in melee
                            pHatefulTarget = mtInMelee ? currentVictim : nullptr;
                        }
                        else
                        {
                            // Hit the highest threat non-MT target in melee (first in list is highest threat)
                            pHatefulTarget = meleeThreats[0]->GetVictim();
                        }
                    }

                    // Fallback: if we somehow have no valid target but MT is in melee, hit MT
                    if (!pHatefulTarget && mtInMelee)
                        pHatefulTarget = currentVictim;

                    // If still no target, skip this cast
                    if (!pHatefulTarget)
                    {
                        events.Repeat(1200ms);
                        break;
                    }

                    // Add threat to highest threat targets (up to top 3)
                    AddThreat(currentVictim, HATEFUL_THREAT_AMT);
                    uint32 threatCount = 0;
                    for (auto threatIt = list.begin(); threatIt != end && threatCount < 2; ++threatIt)
                    {
                        ThreatReference* threatRef = *threatIt;
                        if (threatRef->IsAvailable() && threatRef->GetVictim() != currentVictim)
                        {
                            threatRef->AddThreat(HATEFUL_THREAT_AMT);
                            threatCount++;
                        }
                    }

                    DoCast(pHatefulTarget, SPELL_HATEFUL_STRIKE, true);

                    events.Repeat(1200ms);
                    break;
                }
                case EVENT_BERSERK:
                    DoCastSelf(SPELL_BERSERK, true);
                    Talk(EMOTE_BERSERK);
                    events.ScheduleEvent(EVENT_SLIME, 2s);
                    break;
                case EVENT_SLIME:
                    DoCastAOE(SPELL_SLIME_BOLT, true);
                    events.Repeat(Seconds(2));
                    break;
            }
        }

        if (!_enraged && !HealthAbovePct(5))
        {
            DoCastSelf(SPELL_FRENZY, true);
            Talk(EMOTE_FRENZY);
            _enraged = true;
        }

        DoMeleeAttackIfReady();
    }

private:
    bool _enraged;
};

void AddSC_boss_patchwerk()
{
    RegisterNaxxramasCreatureAI(boss_patchwerk);
}

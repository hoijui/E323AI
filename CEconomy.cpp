#include "CEconomy.h"

CEconomy::CEconomy(AIClasses *ai): ARegistrar(700) {
	this->ai = ai;
	incomes  = 0;
	mNow     = mNowSummed     = eNow     = eNowSummed     = 0.0f;
	mIncome  = mIncomeSummed  = eIncome  = eIncomeSummed  = 0.0f;
	uMIncome = uMIncomeSummed = uEIncome = uEIncomeSummed = 0.0f;
	mUsage   = mUsageSummed   = eUsage   = eUsageSummed   = 0.0f;
	mStorage = eStorage                                   = 0.0f;
}

void CEconomy::init(CUnit &unit) {
	const UnitDef *ud = ai->call->GetUnitDef(unit.key);
	UnitType *utCommander = UT(ud->id);
	mRequest = eRequest = false;

	factory = ai->unitTable->canBuild(utCommander, KBOT|TECH1);
	mex = ai->unitTable->canBuild(utCommander, MEXTRACTOR);
	UnitType *utWind  = ai->unitTable->canBuild(utCommander, EMAKER|WIND);
	utSolar = ai->unitTable->canBuild(utCommander, EMAKER);

	builder = ai->unitTable->canBuild(factory, BUILDER|MOBILE);

	float avgWind   = (ai->call->GetMinWind() + ai->call->GetMaxWind()) / 2.0f;
	float windProf  = avgWind / utWind->cost;
	float solarProf = utSolar->energyMake / utSolar->cost;

	energyProvider  = windProf > solarProf ? utWind : utSolar;
}

CGroup* CEconomy::requestGroup() {
	CGroup *group = NULL;
	int index     = 0;

	/* Create a new slot */
	if (free.empty()) {
		CGroup g(ai);
		groups.push_back(g);
		group = &groups.back();
		index = groups.size()-1;
	}

	/* Use top free slot from stack */
	else {
		index = free.top(); free.pop();
		group = &groups[index];
		group->reset();
	}

	lookup[group->key] = index;
	group->reg(*this);
	return group;
}

void CEconomy::remove(ARegistrar &group) {
	free.push(lookup[group.key]);
	lookup.erase(group.key);
	activeGroups.erase(group.key);

	std::list<ARegistrar*>::iterator i;
	for (i = records.begin(); i != records.end(); i++)
		(*i)->remove(group);
}

void CEconomy::addUnit(CUnit &unit) {
	unsigned c = unit.type->cats;
	if (c&FACTORY) {
		ai->unitTable->factories[unit.key] = false;
		ai->tasks->addFactoryTask(unit);
	}

	else if (c&BUILDER && c&MOBILE) {
		unit.moveForward(-70.0f);
		CGroup *group = requestGroup();
		group->addUnit(unit);
	}

	else if (c&MMAKER) {
		ai->unitTable->metalMakers[unit.key] = false;
	}
}

void CEconomy::update(int frame) {
	/* If we are stalling, do something about it */
	preventStalling();

	/* Update idle worker groups */
	std::map<int, CGroup*>::iterator i;
	for (i = activeGroups.begin(); i != activeGroups.end(); i++) {
		CGroup *group = i->second;
		if (group->busy) continue;

		std::vector<CGroup*> V; V.push_back(group);

		/* Increase eco income */
		UnitType *ut = group->units.begin()->second->type;
		if (stalling || mRequest || eRequest) {
			if (mRequest || mstall) {
				ATask *task = canAssist(BUILD_MPROVIDER, *group);
				if (task != NULL)
					ai->tasks->addAssistTask(*task, V);
				else  {
					float3 pos;
					bool canBuildMex = ai->metalMap->getMexSpot(*group, pos);;
					if (canBuildMex) {
						UnitType *mex = ai->unitTable->canBuild(ut, MEXTRACTOR);
						ai->tasks->addBuildTask(BUILD_MPROVIDER, mex, V, pos);
					}
					else {
						UnitType *mmaker = ai->unitTable->canBuild(ut, MMAKER);
						ai->tasks->addBuildTask(BUILD_MPROVIDER, mmaker, V);
					}
				}
				mRequest = false;
			}
			else if (eRequest || estall) {
				ATask *task = canAssist(BUILD_EPROVIDER, *group);
				if (task != NULL)
					ai->tasks->addAssistTask(*task, V);
				else
					ai->tasks->addBuildTask(BUILD_EPROVIDER, energyProvider, V);
				eRequest = false;
			}
		}
		/* If we can afford to assist a lab and it's close enough, do so */
		else {
			ATask *task = canAssistFactory(*group);
			if (task != NULL)
				ai->tasks->addAssistTask(*task, V);

			else if (eexceeding && mexceeding){
				ATask *task = canAssist(BUILD_FACTORY, *group);
				if (task != NULL)
					ai->tasks->addAssistTask(*task, V);

				else {
					UnitType *factory = ai->unitTable->canBuild(ut, KBOT|TECH2);
					if (factory == NULL)
						factory = ai->unitTable->canBuild(ut, VEHICLE|TECH1);
					ai->tasks->addBuildTask(BUILD_FACTORY, factory, V);
				}
			}
			else {} //TODO: build defense?
		}
	}

	if (ai->unitTable->builders.size() <= 1)
		ai->wl->push(BUILDER, HIGH);

	if (stalling || exceeding)
		ai->wl->push(BUILDER, NORMAL);
}

void CEconomy::preventStalling() {
	std::map<int, bool>::iterator j;

	/* If we aren't stalling, return */
	if (!stalling)
		return;

	/* If we are only stalling energy, see if we can turn metalmakers off */
	if (estall) {
		for (j = ai->unitTable->metalMakers.begin(); j != ai->unitTable->metalMakers.end(); j++) {
			if (j->second) {
				CUnit *unit = ai->unitTable->getUnit(j->first);
				unit->setOnOff(false);
				j->second = false;
				if (!mstall)
					return;
			}
		}
	}

	/* If we are only stalling metal, see if we can turn metalmakers on */
	if (mstall) {
		for (j = ai->unitTable->metalMakers.begin(); j != ai->unitTable->metalMakers.end(); j++) {
			if (!j->second) {
				CUnit *unit = ai->unitTable->getUnit(j->first);
				unit->setOnOff(true);
				j->second = true;
				if (!estall)
					return;
			}
		}
	}

	/* Stop all guarding workers */
	std::map<int,CTaskHandler::AssistTask*>::iterator i;
	for (i = ai->tasks->activeAssistTasks.begin(); i != ai->tasks->activeAssistTasks.end(); i++) {
		/* If the assisting group isn't moving, but actually assisting make them stop */
		if (i->second->isMoving) 
			continue;

		i->second->remove();
		return;
	}
}

void CEconomy::updateIncomes(int frame) {
	/* incomes unreliable before this frame, bah */
	if (frame < 65) return;
	incomes++;

	mNowSummed    += ai->call->GetMetal();
	eNowSummed    += ai->call->GetEnergy();
	mIncomeSummed += ai->call->GetMetalIncome();
	eIncomeSummed += ai->call->GetEnergyIncome();
	mUsageSummed  += ai->call->GetMetalUsage();
	eUsageSummed  += ai->call->GetEnergyUsage();
	mStorage       = ai->call->GetMetalStorage();
	eStorage       = ai->call->GetEnergyStorage();

	mNow     = alpha*(mNowSummed / incomes)    + (1.0f-alpha)*(ai->call->GetMetal());
	eNow     = alpha*(eNowSummed / incomes)    + (1.0f-alpha)*(ai->call->GetEnergy());
	mIncome  = alpha*(mIncomeSummed / incomes) + (1.0f-alpha)*(ai->call->GetMetalIncome());
	eIncome  = alpha*(eIncomeSummed / incomes) + (1.0f-alpha)*(ai->call->GetEnergyIncome());
	mUsage   = alpha*(mUsageSummed / incomes)  + (1.0f-alpha)*(ai->call->GetMetalUsage());
	eUsage   = alpha*(eUsageSummed / incomes)  + (1.0f-alpha)*(ai->call->GetEnergyUsage());

	std::map<int, CUnit*>::iterator i;
	float mU = 0.0f, eU = 0.0f;
	for (i = ai->unitTable->activeUnits.begin(); i != ai->unitTable->activeUnits.end(); i++) {
		unsigned int c = i->second->type->cats;
		if (!(c&MMAKER) || !(c&EMAKER) || !(c&MEXTRACTOR)) {
			mU += i->second->type->metalMake;
			eU += i->second->type->energyMake;
		}
	}
	uMIncomeSummed += mU;
	uEIncomeSummed += eU;

	uMIncome = alpha*(uMIncomeSummed / incomes) + (1.0f-alpha)*mU;
	uEIncome = alpha*(uEIncomeSummed / incomes) + (1.0f-alpha)*eU;

	mstall     = (mNow < 30.0f && mUsage > mIncome);
	estall     = (eNow/eStorage < 0.1f && eUsage > eIncome);
	stalling   = (mstall || estall);

	eexceeding = (eNow > eStorage*0.9f && eUsage < eIncome);
	mexceeding = (mNow > mStorage*0.9f && mUsage < mIncome);
	exceeding  = (mexceeding || eexceeding);
}

ATask* CEconomy::canAssist(buildType t, CGroup &group) {
	std::map<int, CTaskHandler::BuildTask*>::iterator i;
	std::map<float, CTaskHandler::BuildTask*> suited;
	std::map<float, CTaskHandler::BuildTask*>::iterator best;
	float3 pos = group.pos();
	for (i = ai->tasks->activeBuildTasks.begin(); i != ai->tasks->activeBuildTasks.end(); i++) {
		CTaskHandler::BuildTask *buildtask = i->second;

		/* Only build tasks we are interested in */
		if (buildtask->bt != t) continue;

		/* TODO: instead of euclid distance, use pathfinder distance */
		float dist   = (pos - buildtask->pos).Length2D();
		suited[dist] = buildtask;
	}

	/* There are no suited tasks that require assistance */
	if (suited.empty())
		return NULL;

	/* See if we can get there in time */
	best = suited.begin();
	float buildTime  = best->second->toBuild->def->buildTime / (group.buildSpeed/32.0f);
	float travelTime = best->first / (group.speed/30.0f);
	if (travelTime < buildTime)
		return best->second;
	else
		return NULL;
}

ATask* CEconomy::canAssistFactory(CGroup &group) {
	std::map<int, CTaskHandler::FactoryTask*>::iterator i;
	CTaskHandler::FactoryTask *best = NULL;
	float3 pos = group.pos();
	float bestDist = MAX_FLOAT;
	for (i = ai->tasks->activeFactoryTasks.begin(); i != ai->tasks->activeFactoryTasks.end(); i++) {
		/* TODO: instead of euclid distance, use pathfinder distance */
		float dist = (pos - i->second->pos).Length2D();

		if (dist < bestDist) {
			bestDist = dist;
			best = i->second;
		}
	}
	if (best == NULL)
		return NULL;
	UnitType *ut = ai->unitTable->factoriesBuilding[best->factory->key];
	if (ut == NULL)
		return NULL;

	if (canAffordToBuild(group, ut))
		return best;
	else
		return NULL;
}

bool CEconomy::canAffordToBuild(CGroup &group, UnitType *utToBuild) {
	/* NOTE: "Salary" is provided every 32 logical frames */
	float buildTime   = (utToBuild->def->buildTime / (group.buildSpeed/32.0f)) / 32.0f;
	float mCost       = utToBuild->def->metalCost;
	float eCost       = utToBuild->def->energyCost;
	float mPrediction = (mIncome-mUsage)*buildTime - mCost + mNow;
	float ePrediction = (eIncome-eUsage)*buildTime - eCost + eNow;
	if (mPrediction < 0.0f) mRequest = true;
	if (ePrediction < 0.0f) eRequest = true;
	return (mPrediction >= 0.0f && ePrediction >= 0.0f);
}

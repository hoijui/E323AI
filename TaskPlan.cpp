#include "TaskPlan.h"

CTaskPlan::CTaskPlan(AIClasses *ai) {
	this->ai = ai;
	previousFrame = 0;

	taskStr[BUILD_MMAKER]   = std::string("build_mmaker");
	taskStr[BUILD_EMAKER]   = std::string("build_emaker");
	taskStr[BUILD_FACTORY]  = std::string("build_factory");
	taskStr[ASSIST_FACTORY] = std::string("assist_factory");
	taskStr[HARRAS]         = std::string("harras");
	taskStr[ATTACK]         = std::string("attack");
}

void CTaskPlan::addMilitaryPlan(task t, int unitOrGroup, int target) {
	militaryplans[unitOrGroup] = new MilitaryPlan(t, target);
	const UnitDef *ud = ai->cheat->GetUnitDef(target);
	assert(ud != NULL);
	
	sprintf(buf,"[CTaskPlan::addMilitaryPlan]\t <%s, %s>", taskStr[t].c_str(), ud->humanName.c_str());
	LOGN(buf);
}

void CTaskPlan::addBuildPlan(int unit, UnitType *toBuild) {
	const UnitDef *ud = UD(unit);
	task t;

	if (toBuild->cats&FACTORY)
		t = BUILD_FACTORY;
	else if(toBuild->cats&MEXTRACTOR || toBuild->cats&MMAKER)
		t = BUILD_MMAKER;
	else if(toBuild->cats&EMAKER)
		t = BUILD_EMAKER;
	
	
	buildplans[unit] = new BuildPlan(t, toBuild);
	sprintf(buf,"[CTaskPlan::addBuildPlan]\t <%s(%d), %s(%d)>", ud->humanName.c_str(), unit, taskStr[t].c_str(), toBuild->id);
	LOGN(buf);
}

void CTaskPlan::updateBuildPlans(int unit) {
	std::map<int, BuildPlan*>::iterator i;
	std::vector<int> erase;
	const UnitDef *ud = ai->call->GetUnitDef(unit);
	float3 buildedPos = ai->call->GetUnitPos(unit);

	for (i = buildplans.begin(); i != buildplans.end(); i++) {
		BuildPlan *bp = i->second;

		if (ai->eco->gameBuilding[unit] == i->first) {
				erase.push_back(i->first);
				sprintf(buf,"[CTaskPlan::update]\t Remove <%s(%d), %s(%d)>", ai->call->GetUnitDef(i->first)->humanName.c_str(), i->first, taskStr[bp->t].c_str(), bp->toBuild->id);
				LOGN(buf);
		}
	}
	for (unsigned int i = 0; i < erase.size(); i++)
		buildplans.erase(erase[i]);
}

void CTaskPlan::updateMilitaryPlans() {
	std::map<int, MilitaryPlan*>::iterator i;
	std::vector<int> erase;
	
	/* Update military plans */
	for (i = militaryplans.begin(); i != militaryplans.end(); i++) {
		MilitaryPlan *mp = i->second;
		float3 target = ai->cheat->GetUnitPos(mp->target);

		/* Target is destroyed */
		if (target == NULLVECTOR) {
			erase.push_back(i->first);
			ai->military->groups[i->first].busy = false;
			ai->pf->removePath(i->first);
			continue;
		}

		float3 pos  = ai->military->groups[i->first].pos();
		float range = ai->military->groups[i->first].range;

		/* If we are in attack range, start attacking */
		if ((pos - target).Length2D() <= range) {
			ai->metaCmds->attackGroup(ai->military->groups[i->first], mp->target);
			ai->pf->removePath(i->first);
		}
	}

	/* Erase successfully executed plans */
	for (unsigned int i = 0; i < erase.size(); i++) {
		ai->pf->removePath(erase[i]);
		militaryplans.erase(erase[i]);
	}
}

void CTaskPlan::getMilitaryTasks(task t, std::vector<int> &targets) {
	std::map<int, MilitaryPlan*>::iterator i;
	for (i = militaryplans.begin(); i != militaryplans.end(); i++) {
		MilitaryPlan *mp = i->second;
		if (mp->t == t)
			targets.push_back(mp->target);
	}
}

void CTaskPlan::getBuildTasks(task t, std::vector<int> &units) {
	std::map<int, BuildPlan*>::iterator i;
	for (i = buildplans.begin(); i != buildplans.end(); i++) {
		BuildPlan *bp = i->second;
		if (bp->t == t)
			units.push_back(i->first);
	}
}

int CTaskPlan::getTarget(int unitOrGroup) {
	if (militaryplans.find(unitOrGroup) == militaryplans.end()) return -1;
	return militaryplans[unitOrGroup]->target;
}

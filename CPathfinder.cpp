#include "CPathfinder.h"

#include <math.h>
#include <boost/bind.hpp>
#include <boost/version.hpp>

#include "CAI.h"
#include "CTaskHandler.h"
#include "CGroup.h"
#include "CUnit.h"
#include "CUnitTable.h"

CPathfinder::CPathfinder(AIClasses *ai): ARegistrar(600) {
	this->ai   = ai;
	this->X    = int(ai->cb->GetMapWidth()/HEIGHT2SLOPE);
	this->Z    = int(ai->cb->GetMapHeight()/HEIGHT2SLOPE);
	update     = 0;
	repathGroup= -1;

	hm = ai->cb->GetHeightMap();
	sm = ai->cb->GetSlopeMap();

	/* Initialize nodes per map type */
	std::map<int, MoveData*>::iterator i;
	for (i = ai->unittable->moveTypes.begin(); i != ai->unittable->moveTypes.end(); i++) {
		std::vector<Node*> reset;
		std::map<int, Node*> map;
		maps[i->first]       = map;
		activeNodes[i->first] = reset;
		MoveData *md   = i->second;

		for (int z = 0; z < Z; z++) {
			for (int x = 0; x < X; x++) {
				Node *node = new Node(idx(x,z), x, z, 1.0f);
				int j = idx(x,z);

				/* Block too steep slopes */
				if (sm[j] > md->maxSlope) {
					node->setType(BLOCKED);
				}
				/* Block land */
				if (md->moveType == MoveData::Ship_Move) {
					if (hm[j] >= -md->depth)
						node->setType(BLOCKED);
				}
				/* Block water */
				else {
					if (hm[j] <= -md->depth && md->moveType != MoveData::Hover_Move)
						node->setType(BLOCKED);
				}
				/* Store the usefull nodes */
				if ((x % I_MAP_RES == 0 && z % I_MAP_RES == 0) || node->blocked()) {
					int id = (z/I_MAP_RES)*(X/I_MAP_RES)+(x/I_MAP_RES);
					if (node->blocked())
						maps[i->first][idx(x,z)] = node;
					else
						maps[i->first][id] = node;

					if (!node->blocked())
						activeNodes[i->first].push_back(node);
				}
				/* Delete the others */
				else { delete node; }
			}
		}
	}

	/* Define neighbours, yeah this is expensive */
	std::map<int, std::vector<Node*> >::iterator j;
	float maxdist = sqrt(I_MAP_RES*I_MAP_RES + I_MAP_RES*I_MAP_RES)+I_MAP_RES/2;
	for (j = activeNodes.begin(); j != activeNodes.end(); j++) {
		LOG_II("MoveType(" << j->first << ") has " << activeNodes[j->first].size() << " active nodes")
		for (size_t k = 0; k < activeNodes[j->first].size(); k++) {
			Node *parent = activeNodes[j->first][k];

			for (size_t l = 0; l < activeNodes[j->first].size(); l++) {
				Node *node = activeNodes[j->first][l];
				if (parent == node)
					continue;

				/* Determine if this node is a neighbour */
				int dx = (parent->x - node->x);
				int dz = (parent->z - node->z);
				if (sqrt(dx*dx+dz*dz) <= maxdist)
					parent->neighbours.push_back(node);
			}
			assert(parent->neighbours.size() <= 8);
		}
	}

	//drawMap(3);
	draw = false;

	this->REAL = I_MAP_RES*HEIGHT2REAL*HEIGHT2SLOPE;
	X /= I_MAP_RES; Z /= I_MAP_RES;
	LOG_II("Heightmap " << ai->cb->GetMapWidth() << "x" << ai->cb->GetMapHeight())
	LOG_II("Pathmap   " << X << "x" << Z)

	#if (BOOST_VERSION >= 103500)
	nrThreads = boost::thread::hardware_concurrency();
	#else
	nrThreads = 2;
	#endif
	threads.resize(nrThreads-1);
}

void CPathfinder::resetMap(int thread) {
	int size = activeNodes[activeMap].size() / nrThreads;
	int offset = size*thread;
	for (unsigned i = 0; i < size; i++)
		activeNodes[activeMap][i+offset]->reset();
}

void CPathfinder::remove(ARegistrar &obj) {
	ATask *task = dynamic_cast<ATask*>(&obj);
	LOG_II("CPathfinder::remove " << (*task))
	paths.erase(task->group->key);
	groups.erase(task->group->key);
}

void CPathfinder::updateMap(float *weights) {
	std::map<int, std::vector<Node*> >::iterator i;
	for (i = activeNodes.begin(); i != activeNodes.end(); i++) {
		for (size_t j = 0; j < activeNodes[i->first].size(); j++) {
			Node *node = activeNodes[i->first][j];
			node->w = weights[node->id] + sm[node->id]*1.1f;
		}
	}
}

void CPathfinder::updateFollowers() {
	std::map<int, std::vector<float3> >::iterator path;
	std::map<int, CUnit*>::iterator u;
	unsigned groupnr = 0;
	repathGroup = -1;
	/* Go through all the paths */
	for (path = paths.begin(); path != paths.end(); path++) {
		unsigned segment     = 1;
		int     waypoint     = std::min<int>(3, path->second.size()-segment-1);
		CGroup *group        = groups[path->first];
		float maxGroupLength = group->maxLength();
		std::map<float, CUnit*> M;
		/* Go through all the units in a group */
		for (u = group->units.begin(); u != group->units.end(); u++) {
			CUnit *unit = u->second;
			if (group->waiters[unit->key]) {
				unit->wait();
				group->waiters[unit->key] = false;
			}
			float sl1 = MAX_FLOAT, sl2 = MAX_FLOAT;
			float length = 0.0f;
			int s1 = 0, s2 = 1;
			float3 upos = unit->pos();
			/* Go through the path to determine the unit's segment on the path
			 */
			for (segment = 1; segment < path->second.size()-waypoint; segment++) {
				float3 d1  = upos - path->second[segment-1];
				float3 d2  = upos - path->second[segment];
				float l1 = d1.Length2D();
				float l2 = d2.Length2D();
				/* When the dist between the unit and the segment is
				 * increasing: break 
				 */
				length  += (path->second[s1] - path->second[s2]).Length2D();
				if (l1 > sl1 || l2 > sl2) break;
				s1       = segment-1;
				s2       = segment;
				sl1      = l1; 
				sl2      = l2; 
			}
			/* Now calculate the projection of upos onto the line spanned by
			 * s2-s1 
			 */
			float3 uP = (path->second[s1] - path->second[s2]);
			uP.y = 0.0f;
			uP.Normalize();
			float3 up = upos - path->second[s2];
			/* proj_P(x) = (x dot u) * u */
			float3 uproj = uP * (up.x * uP.x + up.z * uP.z);
			/* calc pos on total path */
			float uposonpath = length - uproj.Length2D();
			/* A map sorts on key (low to high) by default */
			M[uposonpath] = unit;
		}
		group->move(path->second[segment+waypoint]);

		/* Set a wait cmd on units that are going to fast, (They can still
		 * attack during a wait) 
		 */
		if (M.size() > 1) {
			float rearval = M.begin()->first;
			/*
			// REAR/FRONT unit debug
			float3 rupos = M.begin()->second->pos();
			float3 rhigh(rupos); rhigh.y += 100.0f;
			ai->cb->CreateLineFigure(rupos, rhigh, 8.0f, 0, 500, 2);
			ai->cb->SetFigureColor(2, 1.0f, 0.0f, 0.0f, 1.0f);

			float3 fupos = (--M.end())->second->pos();
			float3 fhigh(fupos); fhigh.y += 100.0f;
			ai->cb->CreateLineFigure(fupos, fhigh, 8.0f, 0, 500, 3);
			ai->cb->SetFigureColor(3, 0.0f, 1.0f, 0.0f, 1.0f);
			*/


			for (std::map<float,CUnit*>::iterator i = --M.end(); i != M.begin(); i--) {
				CUnit *unit = i->second;
				if (i->first - rearval > maxGroupLength) {
					if (!group->waiters[unit->key]) {
						unit->wait();
						group->waiters[unit->key] = true;
					}
				}
			}
		}
		/* See who will get their path updated by updatePaths() */
		if (update % paths.size() == groupnr)
			repathGroup = path->first;
		groupnr++;
	}
}

void CPathfinder::updatePaths() {
	update++;

	/* nothing to update */
	if (groups.find(repathGroup) == groups.end())
		return;

	float3 start = groups[repathGroup]->pos();
	float3 goal  = ai->tasks->getPos(*groups[repathGroup]);
	if (!addPath(repathGroup, start, goal)) {
		ai->tasks->removeTask(*groups[repathGroup]);
	}
}

bool CPathfinder::addGroup(CGroup &group, float3 &start, float3 &goal) {
	LOG_II("CPathfinder::addGroup " << group)
	groups[group.key] = &group;
	group.reg(*this);
	return addPath(group.key, start, goal);
}

bool CPathfinder::addTask(ATask &task) {
	LOG_II("CPathfinder::addTask " << task)
	groups[task.group->key] = task.group;
	task.reg(*this);
	float3 start = task.group->pos();
	float3 goal = task.pos;
	return addPath(task.group->key, start, goal);
}

bool CPathfinder::addPath(int group, float3 &start, float3 &goal) {
	activeMap = groups[group]->moveType;
	std::vector<float3> path;
	/* Initialize threads */
	for (size_t i = 1; i < nrThreads; i++)
		threads[i-1] = new boost::thread(boost::bind(&CPathfinder::resetMap, this, i));

	/* Reset the nodes of this map using threads */
	resetMap(0);
	for (size_t i = 1; i < nrThreads; i++) {
		threads[i-1]->join();
		delete threads[i-1];
	}

	/* Reset leftovers */
	int rest   = activeNodes[activeMap].size() % nrThreads;
	int offset = activeNodes[activeMap].size() - rest;
	for (unsigned i = 0; i < rest; i++)
		activeNodes[activeMap][i+offset]->reset();

	/* If we found a path, add it */
	bool success = getPath(start, goal, path, group);

	/* Add it when not empty */
	if (success && !path.empty())
		paths[group] = path;

	return success;
}

bool CPathfinder::getPath(float3 &s, float3 &g, std::vector<float3> &path, int group, float radius) {
	/* If exceeding, snap to boundaries */
	int sx  = int(round(s.x/REAL));
	int sz  = int(round(s.z/REAL));
	int gx  = int(round(g.x/REAL));
	int gz  = int(round(g.z/REAL));
	start   = maps[activeMap][idx(sx, sz)];
	goal    = maps[activeMap][idx(gx, gz)];

	std::list<ANode*> nodepath;
	bool success = findPath(nodepath);
	if (success) {
		/* Insert a pre-waypoint at the beginning of the path */
		int waypoint = std::min<int>(3, nodepath.size()-1);
		std::list<ANode*>::iterator wp;
		int x = 0;
		for (wp = nodepath.begin(); wp != nodepath.end(); wp++) {
			if (x >= waypoint) break;
			x++;
		}

		float3 ss  = dynamic_cast<Node*>(*wp)->toFloat3();
		float3 seg = s - ss;
		seg *= 1000.0f; /* Blow up the pre-waypoint */
		seg += s;
		seg.y = ai->cb->GetElevation(seg.x, seg.z)+10;
		path.push_back(seg);

		for (std::list<ANode*>::iterator i = nodepath.begin(); i != nodepath.end(); i++) {
			Node *n = dynamic_cast<Node*>(*i);
			float3 f = n->toFloat3();
			f.y = ai->cb->GetElevation(f.x, f.z)+20;
			path.push_back(f);
		}
		path.push_back(g);

		if (draw) {
			for (unsigned i = 2; i < path.size(); i++) 
				ai->cb->CreateLineFigure(path[i-1], path[i], 8.0f, 0, 500, group);
			ai->cb->SetFigureColor(group, group/float(CGroup::counter), 1.0f-group/float(CGroup::counter), 1.0f, 1.0f);
		}
	}
	else {
		LOG_EE("CPathfinder::getPath pathing failed for " << (*groups[group]))
	}

	return success;
}

float CPathfinder::heuristic(ANode *an1, ANode *an2) {
	Node *n1 = dynamic_cast<Node*>(an1);
	Node *n2 = dynamic_cast<Node*>(an2);
	int dx1 = n1->x - n2->x;
	int dz1 = n1->z - n2->z;
	return sqrt(dx1*dx1 + dz1*dz1)*1.000001f;
}

void CPathfinder::successors(ANode *an, std::queue<ANode*> &succ) {
	Node *n = dynamic_cast<Node*>(an);
	for (size_t u = 0; u < n->neighbours.size(); u++)
		succ.push(n->neighbours[u]);
}

void CPathfinder::drawMap(int map) {
	std::map<int, Node*>::iterator i;
	for (i = maps[map].begin(); i != maps[map].end(); i++) {
		Node *node = i->second; 
		float3 p0 = node->toFloat3();
		p0.y = ai->cb->GetElevation(p0.x, p0.z);
		float3 p1(p0);
		p1.y += 100.0f;
		if (node->blocked()) {
			ai->cb->CreateLineFigure(p0, p1, 10.0f, 1, 100000, 10);
			ai->cb->SetFigureColor(10, 1.0f, 0.0f, 0.0f, 1.0f);
		}
		else {
			ai->cb->CreateLineFigure(p0, p1, 10.0f, 1, 100000, 20);
			ai->cb->SetFigureColor(20, 1.0f, 1.0f, 1.0f, 0.3f);
		}
	}
}

float CPathfinder::gauss(float x, float sigma, float mu) {
	float a = 1.0f / (sigma * sqrt(2*M_PI));
	float b = exp( -( pow(x-mu, 2) / (2*(pow(sigma,2))) ) );
	return a * b;
}

inline int CPathfinder::idx(int x, int z) {
	return z*X+x;
}

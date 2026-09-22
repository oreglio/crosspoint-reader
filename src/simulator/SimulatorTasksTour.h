#pragma once

#ifdef SIMULATOR
// Parcours visuel des ecrans CrossTasks, pilote par l'horloge. Actif seulement
// quand CROSSINK_SIMULATOR_TASKS_TOUR est defini ; sinon ne fait rien. Voir
// scripts/run_simulator_tasks_tour.py, qui prepare la carte et capture chaque
// etape au moment ou elle est stabilisee.
void runSimulatorTasksTourTick();
#endif

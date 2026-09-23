#pragma once

// « Garder l'ecran allume » du menu d'une tache. Vaut pour tout l'espace
// Taches, pas seulement la liste : lire la note d'une tache (ecran de detail,
// pousse au-dessus de la liste) ne doit pas laisser l'appareil s'endormir.
// Remis a faux a chaque entree dans la liste depuis l'accueil
// (TaskListActivity::onEnter), donc quitter les taches rend la veille, et la
// batterie ne paie jamais un oubli. Pas de persistance : une valeur de RAM.
namespace task_keep_awake {
inline bool active = false;
}  // namespace task_keep_awake

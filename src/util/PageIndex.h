#pragma once

// Arithmetique d'index de page, seule et pure : aucun include Arduino, HAL ou
// MappedInputManager, donc ce header est compilable a l'hote. C'est la SEULE
// implementation du saut de page du depot : ButtonNavigator::nextPageIndex /
// previousPageIndex (utilise par la quarantaine d'ecrans de liste) et
// taskListPageJump (ecran des taches, teste a l'hote) delegent tous les deux
// ici. Avant cette extraction, l'ecran des taches en portait une copie que
// rien n'aurait empeche de diverger en silence.
//
// CONTRAT : les deux fonctions supposent `total > 0` et `perPage > 0`. La
// garde des cas degeneres appartient a l'APPELANT, volontairement, parce que
// les deux appelants n'en veulent pas la meme chose : ButtonNavigator rend 0
// sur une liste vide (il n'a pas de notion de "pas de selection"), le modele
// des taches rend -1 (il en a une, et ses quatre handlers la testent).
// Remonter l'une des deux conventions ici casserait l'autre.
//
// `constexpr`/`inline` : aucun cout flash supplementaire, tout est inline sur
// les deux sites d'appel.

// Index de la premiere ligne de la page suivante, avec bouclage. Quand tout
// tient sur une page, avance d'une ligne (meme comportement que
// ButtonNavigator::nextIndex).
constexpr int nextPageIndexPure(const int current, const int total, const int perPage) {
  if (total <= perPage) return (current + 1) % total;

  const int lastPageIndex = (total - 1) / perPage;
  const int currentPageIndex = current / perPage;
  if (currentPageIndex < lastPageIndex) return (currentPageIndex + 1) * perPage;
  return 0;
}

// Index de la premiere ligne de la page precedente, avec bouclage. Quand tout
// tient sur une page, recule d'une ligne (meme comportement que
// ButtonNavigator::previousIndex).
constexpr int previousPageIndexPure(const int current, const int total, const int perPage) {
  if (total <= perPage) return (current + total - 1) % total;

  const int lastPageIndex = (total - 1) / perPage;
  const int currentPageIndex = current / perPage;
  if (currentPageIndex > 0) return (currentPageIndex - 1) * perPage;
  return lastPageIndex * perPage;
}

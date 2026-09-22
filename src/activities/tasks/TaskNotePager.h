#pragma once

#include <cstddef>
#include <cstdint>

// Arithmetique pure de pagination de la note d'une tache : ou couper une
// ligne, et combien de lignes tiennent dans la region bornee de l'ecran de
// detail. Extrait de TaskDetailActivity pour la meme raison que
// TaskListModel l'a ete de TaskListActivity : la suite de tests hote ne peut
// atteindre aucune Activity, donc une propriete qui doit etre epinglee doit
// vivre dans une fonction pure.
//
// Ce fichier ne lit AUCUN fichier et ne connait ni GfxRenderer ni police : il
// travaille sur une fenetre d'octets deja lue par l'appelant et delegue toute
// mesure a un pointeur de fonction (pas un std::function — regle "pas de
// std::function en code de bibliotheque" du CLAUDE.md). C'est aussi ce qui le
// rend compilable a l'hote sans Arduino ni HAL.

// Mesure la largeur, en pixels, des `prefixLen` premiers octets de `window`.
// C'est bien un PREFIXE, pas un mot isole : mesurer le prefixe complet garde
// le crenage et l'avance reelle des espaces, et evite a l'appelant de
// recopier quoi que ce soit — TaskDetailActivity termine temporairement la
// chaine en place dans son propre tampon.
using TaskNoteMeasureFn = int (*)(void* ctx, const char* window, size_t prefixLen);

struct TaskNoteLineBreak {
  // Octets a dessiner, depuis window[0]. Peut valoir 0 : une ligne vide est
  // un paragraphe vide, pas une erreur.
  uint16_t drawBytes = 0;
  // Octets a consommer pour atteindre le debut de la ligne suivante, donc
  // >= drawBytes : la difference est le separateur (espaces, "\n", "\r\n").
  // Vaut 0 seulement quand la fenetre est vide.
  uint16_t skipBytes = 0;
};

// Plus grand prefixe de `window` qui se termine sur une sequence UTF-8
// complete. L'appelant borne avec ca une fenetre qui n'atteint PAS la fin de
// la note, pour que la taille du tampon de lecture ne coupe jamais un
// caractere en deux — ni a l'affichage, ni sur l'octet de depart de la page
// suivante.
size_t taskNoteCompleteUtf8Prefix(const char* window, size_t windowLen);

// Nombre de lignes que la region peut afficher. Jamais moins de 1 : une
// region trop courte doit montrer une ligne tronquee, pas une page vide qui
// ne progresserait jamais.
int taskNoteLinesPerPage(int regionHeight, int advanceY);

// Decoupe la prochaine ligne de `window`.
//
// `windowIsFinal` dit si la fenetre atteint la fin de la note. Quand elle ne
// l'atteint pas, un mot qui touche le bord de la fenetre peut etre coupe en
// deux par la lecture : il n'est alors jamais valide, et la ligne s'arrete au
// mot precedent. C'est ce qui permet a l'appelant de relire une fenetre
// suivante sans jamais couper un mot au hasard de la taille du tampon.
//
// Progression garantie : hors fenetre vide, skipBytes est toujours > 0, y
// compris pour un mot unique plus large que la ligne — celui-la est coupe sur
// une frontiere UTF-8, jamais au milieu d'un caractere.
TaskNoteLineBreak taskNoteNextLine(const char* window, size_t windowLen, bool windowIsFinal, int maxWidth,
                                   TaskNoteMeasureFn measure, void* ctx);

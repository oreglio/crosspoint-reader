#include "TaskNotePager.h"

namespace {

bool isBlank(const char c) { return c == ' ' || c == '\t'; }

size_t utf8SequenceLength(const char c) {
  const unsigned char b = static_cast<unsigned char>(c);
  if ((b & 0xE0) == 0xC0) return 2;
  if ((b & 0xF0) == 0xE0) return 3;
  if ((b & 0xF8) == 0xF0) return 4;
  return 1;
}

// Fin de la sequence UTF-8 qui commence a `i`. Une sequence coupee par le
// bord de la fenetre rend `len` : elle est alors consommee d'un bloc plutot
// que de bloquer la progression. taskNoteCompleteUtf8Prefix() evite ce cas en
// amont pour toute fenetre qui n'atteint pas la fin de la note.
size_t utf8Next(const char* s, const size_t len, const size_t i) {
  if (i >= len) return len;
  const size_t step = utf8SequenceLength(s[i]);
  return (i + step > len) ? len : i + step;
}

// Plus grand prefixe de window[0, limit) qui tient dans maxWidth, aligne sur
// une frontiere UTF-8. Rend au moins un caractere meme s'il deborde : sans
// cela un glyphe plus large que la region ferait boucler la pagination sur
// une ligne vide.
size_t fittingPrefix(const char* window, const size_t limit, const int maxWidth, const TaskNoteMeasureFn measure,
                     void* ctx) {
  size_t best = 0;
  for (size_t k = utf8Next(window, limit, 0); k > best && k <= limit; k = utf8Next(window, limit, k)) {
    if (measure(ctx, window, k) > maxWidth) break;
    best = k;
    if (k == limit) break;
  }
  if (best == 0) best = utf8Next(window, limit, 0);
  if (best == 0) best = limit;  // filet : limit vaut au moins 1 ici
  return best;
}

uint16_t clampToU16(const size_t value) {
  return value > UINT16_MAX ? static_cast<uint16_t>(UINT16_MAX) : static_cast<uint16_t>(value);
}

}  // namespace

size_t taskNoteCompleteUtf8Prefix(const char* window, const size_t windowLen) {
  if (window == nullptr || windowLen == 0) return 0;
  size_t i = 0;
  while (i < windowLen) {
    const size_t step = utf8SequenceLength(window[i]);
    if (i + step > windowLen) return i;
    i += step;
  }
  return windowLen;
}

int taskNoteLinesPerPage(const int regionHeight, const int advanceY) {
  if (advanceY <= 0 || regionHeight <= 0) return 1;
  const int lines = regionHeight / advanceY;
  return lines < 1 ? 1 : lines;
}

TaskNoteLineBreak taskNoteNextLine(const char* window, const size_t windowLen, const bool windowIsFinal,
                                   const int maxWidth, const TaskNoteMeasureFn measure, void* ctx) {
  if (window == nullptr || windowLen == 0 || measure == nullptr) return {};

  // Dernier mot dont le prefixe complet tient encore, et l'endroit ou
  // reprendre apres lui (separateurs avales).
  size_t committedEnd = 0;
  size_t committedSkip = 0;
  bool haveWord = false;

  size_t i = 0;
  while (i < windowLen) {
    if (window[i] == '\n') {
      // Fin de paragraphe : elle ferme la ligne quoi qu'il arrive. Un mot
      // trop large aurait deja fait sortir la boucle plus haut.
      size_t end = i;
      if (end > 0 && window[end - 1] == '\r') end--;
      return {clampToU16(end), clampToU16(i + 1)};
    }
    if (isBlank(window[i])) {
      // Les blancs en tete de fenetre font partie du texte dessine (une
      // indentation se conserve) ; ceux entre deux mots ont deja ete avales
      // par committedSkip.
      i++;
      continue;
    }

    while (i < windowLen && !isBlank(window[i]) && window[i] != '\n') i++;
    const size_t wordEnd = i;

    if (wordEnd == windowLen && !windowIsFinal) {
      // Le mot touche le bord de la fenetre : il continue peut-etre au-dela,
      // donc il ne doit jamais etre valide tel quel.
      if (haveWord) return {clampToU16(committedEnd), clampToU16(committedSkip)};
      const size_t cut = fittingPrefix(window, windowLen, maxWidth, measure, ctx);
      return {clampToU16(cut), clampToU16(cut)};
    }

    if (measure(ctx, window, wordEnd) > maxWidth) {
      if (haveWord) return {clampToU16(committedEnd), clampToU16(committedSkip)};
      // Mot unique plus large que la ligne : coupe dure, sur une frontiere
      // UTF-8. Le reste repart en tete de la ligne suivante.
      const size_t cut = fittingPrefix(window, wordEnd, maxWidth, measure, ctx);
      return {clampToU16(cut), clampToU16(cut)};
    }

    haveWord = true;
    committedEnd = wordEnd;
    size_t next = wordEnd;
    while (next < windowLen && isBlank(window[next])) next++;
    committedSkip = next;
  }

  // Fenetre entierement consommee sans rencontrer de fin de ligne.
  if (!haveWord) return {0, clampToU16(windowLen)};
  return {clampToU16(committedEnd), clampToU16(windowLen)};
}

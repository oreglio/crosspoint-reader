#include "TaskPairActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "MappedInputManager.h"
#include "TaskStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

#if !defined(SIMULATOR)
#include <bootloader_random.h>
#include <esp_system.h>
#endif

namespace {
constexpr char TAG[] = "TASKPAIR";

// Le code est tape a la main : la plus grande police de l'ecran, coupee en
// deux ou trois lignes plutot que reduite.
constexpr int kCodeFontId = LEXENDDECA_16_FONT_ID;
constexpr int kHintFontId = UI_10_FONT_ID;
constexpr int kBlockGap = 8;

// Quatre appels a esp_random(), recopies par memcpy (pas de cast d'un
// uint8_t* non aligne en uint32_t*).
//
// esp_random() ne rend de l'alea vrai que radio allumee (esp_random.h) ; cet
// ecran tourne radio eteinte. bootloader_random_enable() branche le bruit du
// SAR ADC sur le generateur le temps des quatre tirages, pas plus. Pendant
// cette fenetre, AUCUNE lecture ADC ne doit tourner (bootloader_random.h) :
// les boutons des X3/X4 (echelle ADC) et la batterie sont lus par la boucle
// principale — la tache courante — et par les themes pendant un rendu, que
// l'appelant exclut en tenant le verrou de rendu.
void drawSecretBytes(uint8_t (&bytes)[TASK_SECRET_BYTES]) {
#if !defined(SIMULATOR)
  bootloader_random_enable();
#endif
  for (size_t i = 0; i < TASK_SECRET_BYTES; i += sizeof(uint32_t)) {
#if !defined(SIMULATOR)
    const uint32_t value = esp_random();
#else
    // Simulateur seulement : rand() n'est ni cryptographique ni imprevisible.
    const uint32_t value = static_cast<uint32_t>(rand());
#endif
    std::memcpy(bytes + i, &value, sizeof(value));
  }
#if !defined(SIMULATOR)
  bootloader_random_disable();
#endif
}
}  // namespace

// ---------------------------------------------------------------- lifecycle

void TaskPairActivity::onEnter() {
  // Pas de verrou de rendu autour de loadOrCreateSecret() : writeSecret()
  // reecrit tout index.json, et la tache de rendu n'a rien a peindre avant le
  // requestUpdate() final de toute facon.
  Activity::onEnter();
  // AVANT toute lecture du secret : rien ne charge ce store au demarrage, et
  // hasSecret()/readSecret() ne le chargent pas. Sans cet appel, apres un
  // redemarrage, l'ecran voyait un secret vide, en tirait un neuf et ecrasait
  // l'appairage existant en silence. Hors du verrou : lecture SD.
  TASK_STORE.ensureLoaded();
  {
    RenderLock lock(*this);
    computeLayout();
  }
  loadOrCreateSecret();
  // Activity::onEnter() ne programme aucun rendu : sans cet appel l'ecran ne
  // s'afficherait jamais (meme piege que TaskDetailActivity).
  requestUpdate();
}

void TaskPairActivity::onExit() {
  // Efface les copies en clair que cet ecran detient : secret, lignes du code
  // et qrPayload (avant de le rendre au tas). TaskStore efface les siennes
  // (readSecret/writeSecret). NE sont PAS effaces : les tampons internes de
  // l'obfuscation et ceux de QrUtils::drawQrCode (la matrice du QR encode le
  // secret), liberes sans remise a zero hors de portee de cet ecran.
  std::memset(secret, 0, sizeof(secret));
  std::memset(codeLines, 0, sizeof(codeLines));
  std::fill(qrPayload.begin(), qrPayload.end(), '\0');
  qrPayload.clear();
  qrPayload.shrink_to_fit();
  hintLines.clear();
  hintLines.shrink_to_fit();
  // Surtout PAS TASK_STORE.unload() : writeSecret() a charge l'index, et c'est
  // TaskListActivity qui possede son dechargement.
  Activity::onExit();
}

// ------------------------------------------------------------------- secret

void TaskPairActivity::loadOrCreateSecret() {
  char stored[TASK_SECRET_LEN + 2] = {};
  const bool ok = TASK_STORE.readSecret(stored, sizeof(stored)) && taskSecretIsCanonical(stored);
  if (ok) {
    RenderLock lock(*this);
    adoptSecret(stored);
  } else {
    if (TASK_STORE.hasSecret()) {
      LOG_ERR(TAG, "Stored pairing secret is unusable; drawing a new one");
    } else {
      LOG_INF(TAG, "No pairing secret yet; drawing the first one");
    }
    if (!renewSecret()) {
      RenderLock lock(*this);
      writeFailed = true;
    }
  }
  std::memset(stored, 0, sizeof(stored));
}

bool TaskPairActivity::renewSecret() {
  uint8_t bytes[TASK_SECRET_BYTES];
  {
    // Exclut les lectures ADC d'un rendu pendant la fenetre d'entropie.
    RenderLock lock(*this);
    drawSecretBytes(bytes);
  }
  char fresh[TASK_SECRET_LEN + 1];
  taskSecretEncode(bytes, fresh);
  std::memset(bytes, 0, sizeof(bytes));

  // L'ecriture SD hors du verrou : render() ne lit pas le store. Sur echec,
  // le store a garde l'ancien secret : le nouveau n'est PAS affiche, sinon
  // l'utilisateur appairerait un code qu'un redemarrage oublierait.
  const bool saved = TASK_STORE.writeSecret(fresh);
  if (saved) {
    LOG_INF(TAG, "New pairing secret stored");
    RenderLock lock(*this);
    adoptSecret(fresh);
  }
  std::memset(fresh, 0, sizeof(fresh));
  return saved;
}

void TaskPairActivity::adoptSecret(const char* canonical) {
  std::memcpy(secret, canonical, TASK_SECRET_LEN);
  secret[TASK_SECRET_LEN] = '\0';
  std::fill(qrPayload.begin(), qrPayload.end(), '\0');
  qrPayload.assign(secret, TASK_SECRET_LEN);
  layoutCode();
}

// ------------------------------------------------------------------- layout

void TaskPairActivity::computeLayout() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, !mappedInput.hasTouchHardware(), false);
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);

  contentX = safe.x + metrics.contentSidePadding;
  contentWidth = std::max(1, safe.width - 2 * metrics.contentSidePadding);
  hintTop = header.y + header.height + metrics.verticalSpacing;
  // getScreenSafeArea() a deja retire la bande des indices de boutons, dans la
  // bonne dimension selon l'orientation : ne pas la retirer une seconde fois.
  statusTop = safe.y + safe.height - metrics.verticalSpacing - renderer.getLineHeight(SMALL_FONT_ID);

  hintLines = renderer.wrappedText(kHintFontId, tr(STR_TASK_PAIR_HINT), contentWidth, static_cast<int>(kHintMaxLines));
}

void TaskPairActivity::layoutCode() {
  // Coupe glouton entre deux groupes : autant de groupes par ligne que la
  // largeur en laisse passer dans la police du code. Un groupe seul trop large
  // (impossible a 480 px) passerait quand meme sur sa propre ligne.
  char grouped[TASK_SECRET_GROUPED_LEN + 1];
  codeLineCount = 0;
  if (taskSecretGroup(secret, grouped)) {
    const char* cursor = grouped;
    while (*cursor != '\0' && codeLineCount < kMaxCodeLines) {
      char* line = codeLines[codeLineCount];
      size_t length = 0;
      const char* next = cursor;
      while (*next != '\0') {
        const char* groupEnd = std::strchr(next, ' ');
        const size_t groupLen = groupEnd != nullptr ? static_cast<size_t>(groupEnd - next) : std::strlen(next);
        const size_t candidateLen = length == 0 ? groupLen : length + 1 + groupLen;
        char candidate[TASK_SECRET_GROUPED_LEN + 1];
        std::memcpy(candidate, cursor, candidateLen);
        candidate[candidateLen] = '\0';
        if (length > 0 && renderer.getTextWidth(kCodeFontId, candidate) > contentWidth) break;
        std::memcpy(line, candidate, candidateLen + 1);
        length = candidateLen;
        next = groupEnd != nullptr ? groupEnd + 1 : next + groupLen;
      }
      cursor = next;
      ++codeLineCount;
    }
  }
  std::memset(grouped, 0, sizeof(grouped));

  // Le bloc du code est ancre au-dessus de la ligne d'etat ; le QR prend ce
  // qui reste entre la consigne et le code.
  codeTop = statusTop - kBlockGap - static_cast<int>(codeLineCount) * renderer.getLineHeight(kCodeFontId);
  qrTop = hintTop + static_cast<int>(hintLines.size()) * renderer.getLineHeight(kHintFontId) + kBlockGap;
  qrAreaHeight = std::max(0, codeTop - kBlockGap - qrTop);
  qrSize = std::min(contentWidth, qrAreaHeight);
}

// --------------------------------------------------------------------- input

void TaskPairActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
#if CROSSINK_APP_CAP_TOUCH
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finish();
    return;
  }
#endif
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    confirmRenew();
  }
}

void TaskPairActivity::confirmRenew() {
  // Changer le secret desappaire l'appareil cote serveur : chaque sync repond
  // 401 jusqu'a un nouvel appairage sur le web. Un seul appui egare ne doit
  // pas y suffire, d'ou la confirmation commune du depot, qui s'ouvre sur
  // Annuler.
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_TASK_PAIR_RENEW_TITLE),
                                                              tr(STR_TASK_PAIR_RENEW_WARNING));
  if (!confirmation) {
    LOG_ERR(TAG, "OOM: ConfirmationActivity");
    return;
  }
  startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      const bool saved = renewSecret();
      RenderLock lock(*this);
      renewed = saved;
      writeFailed = !saved;
    }
    // Pas de onEnter() au depilement : sans cet appel la fenetre de
    // confirmation resterait peinte par-dessus l'ecran.
    requestUpdate();
  });
}

// -------------------------------------------------------------------- render

void TaskPairActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, tr(STR_TASK_PAIR), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_TASK_PAIR), nullptr);
  }

  const Rect content{contentX, 0, contentWidth, 0};
  int y = hintTop;
  for (const std::string& line : hintLines) {
    UITheme::drawCenteredText(renderer, content, kHintFontId, y, line.c_str());
    y += renderer.getLineHeight(kHintFontId);
  }

  // La charge utile est exactement les 26 caracteres, sans prefixe ni URL :
  // c'est ce que la page du serveur attend dans son champ.
  // drawQrCode() remplit ses bornes sans marge : on lui retire la zone
  // blanche de 4 modules que la norme QR exige autour du symbole. 26
  // caracteres tiennent en version 4 (33 modules, QrUtils.cpp), soit 41
  // modules marge comprise.
  if (qrSize > 0 && !qrPayload.empty()) {
    const int quietZone = qrSize * 4 / 41;
    const int symbol = qrSize - 2 * quietZone;
    const int top = qrTop + (qrAreaHeight - symbol) / 2;
    const Rect qrBounds{contentX + (contentWidth - symbol) / 2, top, symbol, symbol};
    QrUtils::drawQrCode(renderer, qrBounds, qrPayload);
  }

  y = codeTop;
  for (size_t i = 0; i < codeLineCount; ++i) {
    UITheme::drawCenteredText(renderer, content, kCodeFontId, y, codeLines[i]);
    y += renderer.getLineHeight(kCodeFontId);
  }

  // L'echec d'ecriture prime : le code affiche est alors l'ancien, toujours
  // valide, ou aucun au premier appairage.
  const char* status = writeFailed ? tr(STR_TASK_TICK_FAILED) : (renewed ? tr(STR_TASK_PAIR_RENEWED) : nullptr);
  if (status != nullptr) UITheme::drawCenteredText(renderer, content, SMALL_FONT_ID, statusTop, status);

  // Mots seuls, sans glyphe decoratif : un glyphe absent des polices integrees
  // est saute en silence (voir .claude/CONTEXT.md).
  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", tr(STR_TASK_PAIR_RENEW));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Passe complete a chaque image : cet ecran ne se repeint qu'a l'entree et
  // apres « changer », et un QR sans residu d'encre se scanne mieux.
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

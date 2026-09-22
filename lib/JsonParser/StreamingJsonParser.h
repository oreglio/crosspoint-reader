#pragma once

#include <cstddef>
#include <cstdint>

struct JsonCallbacks {
  void* ctx;
  void (*onKey)(void* ctx, const char* key, size_t len);
  void (*onString)(void* ctx, const char* value, size_t len);
  void (*onNumber)(void* ctx, const char* value, size_t len);
  void (*onBool)(void* ctx, bool value);
  void (*onNull)(void* ctx);
  void (*onObjectStart)(void* ctx);
  void (*onObjectEnd)(void* ctx);
  void (*onArrayStart)(void* ctx);
  void (*onArrayEnd)(void* ctx);
};

class StreamingJsonParser {
 public:
  static constexpr size_t TOKEN_BUF_SIZE = 512;
  static constexpr size_t MAX_NESTING = 32;

  explicit StreamingJsonParser(const JsonCallbacks& callbacks);

  void reset();
  void feed(const char* data, size_t len);

  bool hasError() const { return error; }

  // Un champ plus long que TOKEN_BUF_SIZE ne declenche pas d'erreur : le jeton
  // est juste tronque et emitToken() n'appelle onString/onKey/onNumber que
  // pour un jeton complet, donc jamais pour celui-la. Une validation ecrite
  // dans le callback de valeur ne voit alors rien passer et ne peut rien
  // refuser — elle est court-circuitee par au-dessus, pas contournee par en
  // dessous. Un consommateur a qui ca importe doit donc le demander ici
  // plutot que de compter sur son propre callback. Sticky jusqu'a reset() :
  // un depassement peut porter sur un champ que le consommateur ignore,
  // au milieu d'une ligne par ailleurs valide.
  bool tokenTruncated() const { return sawTokenOverflow; }

 private:
  enum class State : uint8_t {
    SCANNING,
    IN_STRING_KEY,
    IN_STRING_VALUE,
    IN_NUMBER,
    IN_LITERAL,
    SKIP_STRING,
  };

  enum class Container : uint8_t {
    NONE,
    OBJECT,
    ARRAY,
  };

  void handleScanning(char c);
  void handleStringChar(char c);
  void handleNumber(char c);
  void handleLiteral(char c);
  void handleSkipString(char c);

  void appendToken(char c);
  void emitToken();

  bool inArray() const { return nestingDepth > 0 && nestingStack[nestingDepth - 1] == Container::ARRAY; }

  JsonCallbacks cb;
  char tokenBuf[TOKEN_BUF_SIZE];
  size_t tokenLen;
  State state;
  bool expectingValue;
  bool escaped;
  bool tokenOverflow;
  bool sawTokenOverflow;
  bool error;

  Container nestingStack[MAX_NESTING];
  uint8_t nestingDepth;

  char literalExpected[6];
  uint8_t literalLen;
  uint8_t literalPos;
};

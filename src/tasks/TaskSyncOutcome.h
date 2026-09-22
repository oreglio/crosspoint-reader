#pragma once

#include <cstdint>

// Ce que la sync a le droit de faire d'une reponse. Une seule issue autorise la
// moindre ecriture sur la carte — curseur, index, notes, file d'ops : Commit.
// Toutes les autres laissent l'etat persiste exactement tel qu'il etait, ce qui
// rend la nouvelle tentative gratuite : les ops sont idempotentes et le curseur
// appartient au serveur. Vider la file pour une reponse qui n'a pas ete
// entierement acceptee est la seule facon de perdre une coche de l'utilisateur.
enum class TaskSyncOutcome : uint8_t {
  Commit,
  // 401 : le secret stocke n'est plus (ou pas encore) celui du serveur. Il est
  // garde tel quel — c'est a l'utilisateur de refaire l'appairage.
  PairingRequired,
  // Connexion, poignee de main TLS, delai depasse ou ligne de statut illisible.
  TransportFailed,
  // Tout statut hors 2xx autre que 401 (5xx, 400, 413, 3xx...).
  ServerError,
  // 2xx mais corps coupe par le transport, ou dont le cadrage est casse ou
  // inacheve (TaskSyncReader::isComplete() faux).
  BadResponse,
};

// httpCode : statut HTTP, ou negatif quand le transport a echoue avant.
// bodyComplete : le transport a lu tout le corps annonce (Content-Length ou
// dernier bloc chunked). readerComplete : TaskSyncReader::isComplete().
constexpr TaskSyncOutcome classifyTaskSyncResponse(int httpCode, bool bodyComplete, bool readerComplete) {
  if (httpCode < 0) return TaskSyncOutcome::TransportFailed;
  if (httpCode == 401) return TaskSyncOutcome::PairingRequired;
  if (httpCode < 200 || httpCode >= 300) return TaskSyncOutcome::ServerError;
  if (!bodyComplete || !readerComplete) return TaskSyncOutcome::BadResponse;
  return TaskSyncOutcome::Commit;
}

#pragma once

#include <cstddef>
#include <cstdint>

// Secret d'appairage : 16 octets tires du generateur materiel, ecrits en 26
// caracteres Crockford base32 (5 bits par caractere, le dernier n'en porte
// que 3). Pur et sans HAL pour que la suite hote puisse l'epingler : un
// encodeur faux produit un code que le serveur n'acceptera jamais, sans la
// moindre erreur cote appareil.
//
// FORME CANONIQUE = exactement 26 caracteres de l'alphabet, en majuscules,
// sans separateur. C'est la SEULE forme qui doit etre stockee et envoyee.
// Le serveur normalise le code tape sur sa page web (majuscules, separateurs
// retires) mais hache l'en-tete `Authorization: Bearer` tel quel
// (crossdrop src/taskApi.ts:47-51 et :172) : un espace ou une minuscule dans
// le secret stocke et chaque sync repond 401 alors que l'appairage web a
// reussi. Le groupement par quatre est un affichage, rien d'autre.

inline constexpr size_t TASK_SECRET_BYTES = 16;
inline constexpr size_t TASK_SECRET_LEN = 26;
// 7 groupes de 4 (le dernier de 2), donc 6 separateurs.
inline constexpr size_t TASK_SECRET_GROUPED_LEN = TASK_SECRET_LEN + 6;

// Pas de I, L, O ni U : ce sont les lettres qu'un humain confond avec 1, 0 et V.
inline constexpr char TASK_SECRET_ALPHABET[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

// Ecrit la forme canonique et son terminateur dans `out`, qui doit tenir
// TASK_SECRET_LEN + 1 octets. Bits pris poids fort d'abord, comme RFC 4648 :
// seul l'alphabet differe.
void taskSecretEncode(const uint8_t bytes[TASK_SECRET_BYTES], char out[TASK_SECRET_LEN + 1]);

// Vrai si `s` est exactement une forme canonique. Garde-fou avant d'afficher
// ou d'envoyer un secret relu de la carte.
bool taskSecretIsCanonical(const char* s);

// Forme d'affichage : groupes de quatre separes d'une espace. `out` doit
// tenir TASK_SECRET_GROUPED_LEN + 1 octets. Rend false (et une chaine vide)
// si `canonical` n'est pas canonique.
bool taskSecretGroup(const char* canonical, char out[TASK_SECRET_GROUPED_LEN + 1]);

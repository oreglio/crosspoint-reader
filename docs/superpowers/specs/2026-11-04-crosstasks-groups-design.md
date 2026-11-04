# CrossTasks — Groupes et « faites » dans l'en-tête — Design

Date : 2026-11-04 · Statut : validé avec l'utilisateur (cette session) ·
Cible : firmware `crosspoint-reader` + serveur/web `crossdrop` · S'appuie sur
`docs/superpowers/specs/2026-09-22-crosstasks-design.md` (le design d'origine,
dont toutes les contraintes restent en vigueur).

Les maquettes validées sont dans `.superpowers/brainstorm/` à la racine
`xteink/` (local, hors git) : `done-header.html`, `groups-device-v2.html`,
`groups-web.html`. Ce document reprend tout ce qu'elles fixent ; elles ne sont
pas nécessaires pour construire.

Légende : **[utilisateur]** = décidé par l'utilisateur ; **[choix de
conception]** = décidé pendant la rédaction, à confirmer en relecture ; le
reste est laissé au constructeur (section dédiée).

## Pourquoi

La liste mélange des tâches de natures différentes (maison, administratif,
travail…). L'utilisateur veut les **ranger visuellement** en groupes libres,
sans toucher au sens de la priorité. En parallèle, l'accordéon « N faites » en
bas de liste sur la liseuse prend une ligne et oblige à descendre tout en bas ;
l'utilisateur veut le remonter dans l'en-tête.

Succès : sur la liseuse, on voit d'un coup d'œil ses tâches par groupe, on
saute de groupe en groupe, on ajoute une tâche directement dans un groupe ; sur
le web, on range une tâche en la glissant dans un groupe.

## Partie 1 — « N faites » dans l'en-tête (liseuse uniquement)

Le web ne change pas pour cette partie.

1. **[utilisateur]** Le compteur « N faites » quitte le bas de la liste et se
   place à droite de l'en-tête, en face de « Tâches (M) ». La ligne accordéon
   `TaskRowKind::DoneSection` disparaît, ainsi que l'état `showDone` « déplié
   sous la liste ».
2. **[utilisateur]** Depuis la première ligne de la liste, **Haut** amène le
   curseur sur le compteur de l'en-tête (surligné comme une ligne
   sélectionnée). Confirmer porte alors le libellé « Voir ».
3. **[utilisateur]** **Confirmer** sur le compteur (ou un appui tactile sur
   le compteur, sur les appareils tactiles) **remplace** la liste par les
   tâches faites. Ce n'est pas le simple passage du curseur qui bascule.
4. **[utilisateur]** Dans la vue « faites », l'en-tête devient « Faites (N) »
   avec « M à faire » à droite ; le même geste (Haut jusqu'au compteur, puis
   Confirmer) ramène à la liste ouverte.
5. Dans la vue « faites », les tâches gardent le comportement actuel des
   tâches faites : Confirmer décoche (la tâche quitte la vue), Droite ouvre
   le détail, maintenir Confirmer ouvre le menu, le mode sélection et
   « Supprimer les faites (N) » restent disponibles. Elles ne sont **pas**
   groupées (voir partie 2) ; leur ordre est celui d'aujourd'hui
   (`taskOrderBefore` : priorité, rang, id).
6. **[choix de conception]** Aucune tâche faite : pas de compteur dans
   l'en-tête, et Haut depuis la première ligne garde son comportement actuel.
   Si la vue « faites » se vide (dernière décochée), on revient
   automatiquement à la liste ouverte.
7. **[choix de conception]** Dans la vue « faites », **Retour** ramène à la
   liste ouverte (et non hors de Tasks), car on l'ouvre comme une sous-vue.
8. **[choix de conception]** L'indicateur « ↻ » (ops en attente de sync)
   occupe aujourd'hui ce même emplacement droit de l'en-tête
   (`TaskListActivity::drawChrome`, passé comme `subtitle`). Les deux
   coexistent dans le même libellé : « 3 faites ↻ ». Largeur à mesurer
   (limite 200 px en `inter_8_regular`, cf. `.claude/CONTEXT.md`).

## Partie 2 — Groupes (liseuse + web + serveur)

### Ce qu'est un groupe

- **[utilisateur]** Un groupe est une étiquette libre, nommée par
  l'utilisateur (ex. MAISON, ADMINISTRATIF, WORK), qui sert **uniquement à
  ranger visuellement**. Il ne remplace pas la priorité : la priorité reste
  marquée par le gras, comme aujourd'hui.
- **[utilisateur]** Une tâche appartient à **au plus un** groupe. Les tâches
  sans groupe existent et sont la valeur par défaut.
- **[utilisateur]** Les groupes ont un ordre, réglé sur le web, identique sur
  la liseuse.
- **[utilisateur]** Supprimer un groupe ne supprime aucune tâche : ses tâches
  repassent **sans groupe**.
- **[choix de conception]** Le nom est affiché tel que saisi (pas de passage
  en capitales côté liseuse : la mise en majuscules UTF-8 accentuée n'y est
  pas fiable). Nom non vide, ≤ 32 octets, sans caractère de contrôle, unique
  sans tenir compte de la casse. **16 groupes maximum.**
- **[choix de conception]** Les tâches faites ne sont pas groupées à
  l'affichage (vue « faites » sur la liseuse, accordéon du bas sur le web),
  mais **gardent** leur groupe : décocher une tâche la remet dans son groupe.

### Ordre d'affichage (liseuse et web, liste ouverte)

1. **[utilisateur]** Les tâches **sans groupe en tête**, sans titre de
   section.
2. Puis chaque groupe, dans l'ordre des groupes, précédé de son titre.
3. **[utilisateur]** Dans chaque bloc : l'ordre actuel — priorité (haute,
   donc grasse, d'abord), puis rang manuel, puis id.

Le rang manuel (`position` serveur / `order` liseuse) vaut désormais **dans un
couple (groupe, priorité)** et non plus dans une priorité seule. Une tâche qui
change de groupe ou de priorité part au bout de sa nouvelle bande, sauf si le
web fournit un rang explicite (glisser-déposer à un endroit précis).

### Liseuse

Maquette : `groups-device-v2.html`.

- **[utilisateur]** Le titre de groupe est une ligne de la liste, en petite
  police grasse espacée, avec le nombre de tâches **ouvertes** du groupe à
  droite et un filet dessous. **[utilisateur]** Un peu plus d'air au-dessus
  du titre qu'entre deux tâches, pour une cohérence d'espacement.
- Le titre est **sélectionnable** (le curseur s'y pose comme sur une tâche).
  Comme l'actuelle ligne « N faites », ce ne peut pas être un
  `ListItem::isHeader` du SDK, qui n'est ni sélectionnable ni touchable
  (commentaire `TaskListActivity.cpp:264`).
- **[utilisateur]** Boutons quand le curseur est sur un titre de groupe :

  | Retour | Confirmer | Gauche | Droite |
  |---|---|---|---|
  | Retour | **Ajouter** | **Précédent** | **Suivant** |

  - Confirmer « Ajouter » : clavier pour le titre, puis choix de la priorité
    (le flux d'ajout actuel), et la tâche est créée **dans ce groupe**, au
    bout de sa bande.
  - Gauche / Droite : saut au titre de groupe précédent / suivant.
    **[choix de conception]** Pas de bouclage : sur le premier groupe,
    Précédent est vide ; sur le dernier, Suivant est vide (case effacée par
    `drawButtonHints`, comme aujourd'hui pour les libellés sans action).
  - Tactile **[choix de conception]** : toucher un titre de groupe = Ajouter.
- **[utilisateur / maquette validée]** Sur une tâche, **Gauche « Nouv. »**
  crée une tâche **sans groupe**, comme aujourd'hui.
- **[utilisateur]** On peut changer le groupe d'une tâche depuis la liseuse.
  **[choix de conception]** Par une entrée « Groupe » dans le menu de la tâche
  (maintenir Confirmer) et sur la page de détail, qui ouvre la liste des
  groupes existants plus « Sans groupe ». L'entrée n'apparaît que s'il existe
  au moins un groupe.
- **[choix de conception]** La liseuse ne **crée, ne renomme, ne supprime ni
  ne réordonne** de groupes : tout cela se fait sur le web.
- **[choix de conception]** Un groupe sans tâche ouverte n'est pas affiché
  sur la liseuse (rien à y lire). Conséquence assumée : on ne peut pas
  « Ajouter » dans un groupe vide depuis la liseuse.
- **[choix de conception]** Le mode sélection traverse les groupes. Le curseur
  **saute** les titres de groupe en mode sélection, parce que Gauche et Droite
  y signifient Effacer et Cocher.
- **[choix de conception]** Basculer entre la vue ouverte et la vue « faites »
  en cours de sélection retire de la sélection les tâches qu'on ne voit plus
  (même règle qu'aujourd'hui quand on replie « N faites »,
  `toggleDoneSection`). Une action ne touche jamais une ligne invisible.

### Web

Maquette : `groups-web.html`.

- **[utilisateur]** **Glisser-déposer** : on attrape une tâche par sa poignée
  ⠿ (existante) et on la dépose dans un groupe. Elle se range dans la bande de
  sa priorité au sein de ce groupe ; la règle actuelle « on ne franchit pas une
  priorité » (`onMove`, `list.html:214`) devient « on peut changer de groupe,
  jamais de priorité ». Déposer la tâche dans la zone du haut (sans groupe) la
  retire de son groupe.
- **[choix de conception]** Bouton **« + Groupe »** dans la barre d'outils :
  ouvre un champ de nom. Le groupe n'est créé, à la fin de l'ordre, qu'une fois
  un nom valide et unique saisi. Pas de nom provisoire du type « Nouveau
  groupe 2 ». L'unicité ignore la casse (`toLocaleLowerCase('fr')`) ; les
  accents restent distincts.
- Déposer une tâche dans un groupe sans tâche de sa priorité, ou dans la zone
  « sans groupe » vide, doit offrir une cible visible qui la range dans sa
  bande de priorité.
- **[choix de conception]** Titre de groupe : clic sur le nom = renommer en
  place ; **+** à droite = ajouter une tâche dans ce groupe ; poignée ⠿ = faire
  glisser le groupe entier pour réordonner les groupes ; une action
  « Supprimer le groupe » avec la confirmation `<dialog>` existante (« Ses N
  tâches repassent sans groupe »).
- **[choix de conception]** Volet de détail : un sélecteur **« Groupe »**
  (groupes + « Sans groupe »), alternative au glisser-déposer, utile sur
  mobile où le volet passe en plein écran.
- **[choix de conception]** Un groupe vide reste affiché sur le web (titre
  seul) pour qu'on puisse y déposer une tâche.
- Les faites restent dans l'accordéon du bas, sans groupes.

### Serveur et synchro

**[choix de conception]** Les groupes sont des objets synchronisés à part
entière, sur le même compteur `task_seq` que les tâches, afin qu'un seul
curseur opaque couvre les deux. Renommer ou réordonner un groupe ne touche
donc que le groupe, pas chacune de ses tâches.

- Nouvelle table, migration additive (même rituel que `migratePosition`,
  `crossdrop/src/taskDb.ts:53`) :
  `task_groups(id TEXT PK 'g'+8hex, name TEXT, position INTEGER, deleted
  INTEGER, updated_at TEXT, seq INTEGER)` ; colonne `tasks.group_id TEXT NULL`.
  Tombstones de groupes purgés comme ceux des tâches, sous le même
  `task_tombstone_floor`.
- **Supprimer un groupe** : tombstone du groupe **et** `group_id = NULL` sur
  ses tâches, chacune avec un `seq` neuf, pour que la liseuse les reçoive.
  La liseuse applique aussi la règle localement à la réception du tombstone
  (si elle le voit avant les tâches, elle ne garde pas de référence morte).
- **Réponse de sync** : de nouvelles lignes NDJSON pour les groupes, à
  distinguer des lignes de tâches, par exemple
  `{"group":"g1a2b3c4","name":"MAISON","position":2}` et
  `{"group":"g1a2b3c4","deleted":true}` (forme exacte au constructeur). Les
  lignes de tâches portent leur `group` (id ou absent). Une tâche peut arriver
  avant son groupe ; la liseuse doit l'afficher sans groupe tant que le groupe
  est inconnu, puis la ranger quand il arrive.
- **Ops de la liseuse** : `add` accepte un `group` optionnel ; nouvelle op
  `{"op":"group","id":"…","group":"g…"|null}`. Un groupe inconnu ou supprimé
  côté serveur n'est **pas** un refus : la tâche passe sans groupe et l'op est
  acceptée (rien à faire pour l'utilisateur).
- **Compatibilité** : la requête porte déjà `"schema": 1`. La liseuse à jour
  envoie `"schema": 2`. Le serveur ne sert lignes de groupes et champ `group`
  **qu'aux requêtes schema ≥ 2** ; un appareil schema 1 continue de voir une
  liste plate, sans erreur. Le serveur est déployé en premier ; la liseuse doit
  aussi fonctionner face à un serveur qui ne renvoie aucun groupe.
- **[choix de conception] Premier passage en schema 2** : le firmware actuel
  refuse toute réponse dont l'en-tête n'est pas `schema: 1`
  (`TaskSyncReader.cpp:337`). Le serveur répond donc `schema: 1` aux requêtes
  schema 1, et le nouveau firmware accepte une réponse `schema: 1` d'un serveur
  pas encore à jour. Entre le déploiement du serveur et le flash de la
  liseuse, celle-ci synchronise en schema 1 et son curseur dépasse les `seq`
  des groupes et affectations créés entre-temps. À sa **première** sync en
  schema 2, la liseuse efface son curseur stocké pour obtenir un instantané
  complet. Aucune op n'est perdue, puisque le serveur applique les ops avant de
  calculer le diff (`applyOps` avant `snapshot`/`changesSince`, `taskApi.ts`).
- **[choix de conception]** Si la liseuse reçoit plus de 16 groupes (serveur
  plus permissif à l'avenir), elle garde les 16 premiers dans l'ordre et
  affiche les tâches des autres sans groupe, avec un `LOG_ERR`.
- Le rang `position` / `endPosition` (`taskDb.ts:77`) passe au couple
  (groupe, priorité). L'API web de réordonnancement
  (`POST /web/api/tasks/reorder`, aujourd'hui `{priority, ids}`) doit pouvoir
  exprimer un changement de groupe avec un rang précis ; forme laissée au
  constructeur.

### Budget mémoire liseuse (contrainte dure)

L'index de 120 `TaskRecord` pèse déjà ~26 Ko, chargé en `onEnter()` et rendu
en `onExit()` (`TaskListActivity.cpp:162-178`). Les groupes suivent la même
règle : **rien de résident hors de l'écran Tasks**, aucun coût au démarrage ni
pendant la lecture. Budget visé pour la partie 2 : **≤ 2 Ko de plus** à l'écran
(16 groupes × nom 32 octets + id ≈ 0,7 Ko, plus la référence de groupe par
tâche). Le fichier sur carte peut porter l'id de groupe en clair ; en RAM, un
petit index de slot (uint8) suffit.

## Non-objectifs

- Replier / déplier un groupe.
- Couleurs, icônes ou emoji de groupe.
- Plusieurs groupes par tâche, sous-groupes.
- Grouper les tâches faites.
- Gérer les groupes (créer, renommer, supprimer, réordonner) depuis la liseuse.
- Changer quoi que ce soit au sens ou à l'affichage de la priorité.

## Anti-objectifs (échec même si « ça marche »)

- Une allocation qui survit à la sortie de Tasks, ou un coût sur le chemin de
  lecture (règle du design d'origine, « Scope note »).
- Une liseuse déjà appairée qui casse ou perd ses ops en attente parce que le
  serveur a été mis à jour avant elle.
- Un groupe supprimé qui fait disparaître des tâches, ou une tâche qui reste
  invisible parce qu'elle référence un groupe inconnu.
- Des libellés qui débordent en français (piège déjà rencontré, voir
  `.claude/CONTEXT.md`) : « Précédent », « Ajouter », « 3 faites ↻ », « M à
  faire ».

## Contraintes

- Toutes celles de `CLAUDE.md` du firmware : C3 d'abord, pas de PSRAM ni de
  tactile supposés, `tr(STR_*)` pour tout libellé, nouvelles clés dans
  `english.yaml` **et** `french.yaml`, entrée `CHANGELOG.md`.
- Toutes celles du design d'origine : sync manuelle, ops rejouables
  (idempotentes par clé primaire), curseur opaque, limites (50 ops/requête,
  200 lignes/page, 120 tâches actives).
- Mise à jour de `docs/crosstasks.md` (liseuse) et du README `crossdrop`.

## Laissé au constructeur

- Forme exacte des lignes NDJSON de groupe et du champ `group`, de la nouvelle
  route web de déplacement/réordonnancement, et des routes CRUD des groupes.
- Fichier de stockage des groupes sur la carte (dans `index.json` ou à part).
- Mécanique de sélection du compteur dans l'en-tête (dessin dans `drawChrome`
  ou évolution de `TouchHeaderBackButton` / du SDK `freeink-sdk`) et du
  comportement de Haut/Bas autour de lui (bouclage ou non au-delà).
- Comment « Ajouter » dans un groupe sur le web (champ en ligne sous le groupe
  ou champ du haut pré-réglé sur le groupe).
- Ordre de livraison ; suggestion : partie 1 seule d'abord (firmware pur, sans
  dépendance serveur), puis serveur + web groupes, puis firmware groupes.

## Vérification attendue

- Serveur : tests vitest pour la migration, le CRUD groupes, la suppression
  (tâches libérées et re-servies), les ops `add`/`group`, la compatibilité
  schema 1 vs 2, le rang par (groupe, priorité).
- Firmware : tests `test/task_list_model` étendus (ordre sans-groupe → groupes,
  titres, sauts Précédent/Suivant, vue « faites ») ; `test/task_op_queue` pour
  la nouvelle op ; e2e `scripts/run_simulator_tasks_sync_e2e.py` ; tournée
  `scripts/run_simulator_tasks_tour.py` dans les trois thèmes, FR et EN ;
  `pio run -e default`, `pio check -e default`.
- Matériel (X4) : sync avec un serveur à jour, un groupe renommé puis supprimé
  sur le web, vérifier que les tâches suivent ; heap libre à l'entrée et à la
  sortie de Tasks identique à avant la fonctionnalité, à l'arrondi près.

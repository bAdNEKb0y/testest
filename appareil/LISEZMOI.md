# Nexxus — le boîtier (Waveshare ESP32-S3 AMOLED 1,75")

Le boîtier, c'est le **corps** : le visage, les oreilles et la voix. Le **cerveau**, lui, tourne sur ton serveur (`relay.php` sur nexxus.pm), qui interroge Claude avec le modèle le moins cher.

## 1. Mettre à jour le serveur (une seule fois)

1. Remplace `relay.php` sur nexxus.pm par la nouvelle version. Recopie tes clés dedans : `$ANTHROPIC_KEY`, `$PRIM_KEY`, `$AZURE_KEY`, `$AZURE_REGION` et `$PHRASE_SECRETE`.
2. Remplis **`$CODE_APPAREIL`** avec un code d'au moins 12 caractères. La page de réglages du boîtier peut t'en créer un avec le bouton « Créer ».
3. **`$AZURE_KEY` est indispensable** pour que le boîtier comprenne ta voix. Elle sert aussi pour qu'il parle. Pour l'obtenir : portal.azure.com → Créer une ressource → « Speech » → offre gratuite F0. La clé et la région se trouvent dans « Clés et point de terminaison ».

## 2. Installer le programme (une seule fois, avec un câble)

1. Branche la carte au PC avec un câble USB-C **qui transfère des données**.
2. Ouvre **Chrome** sur https://espressif.github.io/esptool-js/ puis clique sur **Connect** et choisis le port (souvent « USB JTAG/serial debug unit »).
   - Si rien n'apparaît : débranche, maintiens le bouton **BOOT**, rebranche, puis relâche.
3. Dans la ligne « Flash Address », mets **0x0** et choisis le fichier **`nexxus-complet.bin`**.
4. Clique sur **Program**, attends 100 %, puis appuie sur le bouton de reset (ou débranche et rebranche la carte).

Ensuite, les mises à jour se font **sans câble**, depuis la page de réglages, avec le fichier `nexxus-maj.bin`.

## 3. Premier démarrage

1. L'écran affiche un **QR code**. Scanne-le avec l'appareil photo du téléphone pour rejoindre le Wi-Fi « Nexxus-XXXX ».
2. La page de réglages s'ouvre toute seule. Sinon, va sur `192.168.4.1`.
3. Dans **Wi-Fi**, ajoute ta box **et** le partage de connexion de ton téléphone. Le boîtier prendra tout seul celui qui est disponible (seulement en 2,4 GHz).
4. Une fois connecté, l'écran montre son adresse et un code à 6 chiffres.

## 4. Brancher le cerveau

Ouvre **http://nexxus.local** (ou l'adresse affichée), avec l'identifiant `nexxus` et le code à 6 chiffres. Ensuite :

1. Dans « Adresse du relais », mets `https://nexxus.pm/relay.php` (ou le chemin réel de ton fichier).
2. Dans « Code appareil », mets le même code que `$CODE_APPAREIL`.
3. Clique sur **Enregistrer**, puis sur **Tester**. Chaque ligne doit afficher ✓.

## 5. Les gestes

| Geste | Effet |
|---|---|
| Toucher le visage (ou le bouton BOOT) | Il t'écoute. Parle, puis tais-toi : il comprend tout seul que tu as fini. |
| Toucher pendant qu'il parle | Il se tait et t'écoute. |
| Appui long sur le visage | Câlin 😍 |
| Fiche ouverte : glisser vers le haut ou vers le bas | Faire défiler le contenu. |
| Fiche ouverte : tirer vers le bas | Fermer la fiche. |
| Plusieurs pages (petits points en bas) : glisser sur le côté | Page suivante. |
| Après avoir fermé : glisser vers le haut | Revoir la dernière fiche. |
| Quiz : toucher une réponse | Répondre. |
| Petit visage au-dessus d'une fiche : le toucher | Poser une autre question. |
| Secouer le boîtier | Il réagit 😮 |

Les infos n'apparaissent **que quand tu les demandes**, par exemple :

- « c'est quoi un trou noir ? »
- « prochain métro à Hôtel de Ville vers Saint-Paul »
- « la météo de demain »
- « les infos du jour »
- « fais-moi un quiz »
- « quelle heure est-il ? »
- « tes infos »
- « mets un minuteur de 10 minutes »

**Veille** : sans activité, il montre une grande horloge, la météo et de petits yeux. Le tout bouge un peu chaque minute pour ne pas marquer l'écran. Le jour, l'écran s'éteint 30 minutes plus tard. La nuit, il garde une petite horloge très sombre. Un toucher le réveille.

## 6. Bluetooth

Le boîtier fait du Bluetooth basse consommation. Il ne peut **pas** partager Internet ni servir d'enceinte : pour Internet, utilise le partage de connexion **Wi-Fi** du téléphone.

1. Installe une application « UART Bluetooth » : *Serial Bluetooth Terminal* (Android) ou *Bluefruit Connect* (iPhone).
2. Connecte-toi à « Nexxus ». Le code s'affiche alors sur l'écran du boîtier.
3. Envoie `code 123456` (avec ton code), puis par exemple :
   - `wifi MaBox;motdepasse`
   - `? c'est quoi la plus haute montagne`
   - `dis bonjour`
   - `humeur content`
   - `volume 60`
   - `carte heure`
   - `etat`
   - `tais-toi`

## Pour les curieux

- Le code est dans `src/`. Chaque modification poussée sur GitHub est compilée automatiquement, et les fichiers `.bin` apparaissent dans la branche `firmware`.
- Le dossier `lib/` contient des copies réduites des pilotes de la carte (écran CO5300, codec ES8311, QR code), avec leurs licences.

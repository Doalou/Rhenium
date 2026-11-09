# Construction automatique

Le workflow `.github/workflows/paquet.yml` construit l'app et la publie. Il tourne sur le PC de développement, par un runner GitHub auto-hébergé : l'exécutable de l'app contient Chromium entier, et sa compilation complète demande quelque 330 heures de CPU, bien au-delà des 6 heures d'une tâche sur les serveurs de GitHub. Sur le PC, l'arbre déjà compilé ne recompile que ce qui a changé.

## Deux dépôts

- Le dépôt **privé**, où l'on travaille, a le runner. Seuls ses collaborateurs peuvent y pousser ; aucun code extérieur n'atteint donc le PC ni la clé qui signe l'app.
- Le dépôt **public** reçoit `main`, les tags et les Releases. Il n'a pas de runner, et sa copie du workflow ne fait rien (la tâche ne tourne que dans un dépôt privé). Une contribution reçue en public se reprend à la main dans le dépôt privé.

## Ce que fait le workflow

`paquet.ps1` :

1. récupère uBlock Origin Lite et PDF.js aux versions fixées ;
2. lance `maj-chromium\build-content.ps1` : correctifs, gn, ninja, coquille XAML ;
3. empaquette par `j2\package-shell.ps1`, avec le certificat de `F:\Rhenium\sonde\certs` ;
4. réunit `RheniumShell_arm.appx`, `RheniumDev.cer` et `LISEZMOI.txt` (la notice, tirée de `notice.txt`).

Ensuite, pour un tag `v*` :

- `miroir.ps1` pousse `main` et le tag dans le dépôt public, jamais en force ;
- `publier.ps1` y crée la Release du tag avec les trois fichiers, la notice comme texte.

Un lancement à la main (onglet Actions du dépôt privé, « Paquet Rhénium », Run workflow) garde seulement le paquet dans l'exécution, 30 jours.

## Mise en place, une fois

Dépôt public :

1. Créer le dépôt public sur GitHub, vide (sans README ni licence, pour que la première copie passe).
2. Dans ses Settings, Actions, General : « Disable actions ». Rien n'a à y tourner.

Jeton :

3. Sur GitHub, Settings du compte, Developer settings, Personal access tokens, Fine-grained tokens, Generate new token. Accès au seul dépôt public ; permissions **Contents** et **Workflows** en lecture et écriture (Workflows : la copie contient `.github/workflows`). Choisir une expiration et la noter : à échéance, la publication échoue jusqu'au remplacement du secret.

Dépôt privé, Settings, Secrets and variables, Actions :

4. Secret `RHENIUM_PUBLIC_TOKEN` : le jeton.
5. Variable `RHENIUM_PUBLIC_REPO` : le nom du dépôt public, par exemple `Doalou/Rhenium-public`.

Runner, dans le dépôt privé :

6. Settings, Actions, Runners, New self-hosted runner, Windows x64.
7. Suivre les commandes affichées dans PowerShell, dans un dossier sans espace ni accent, par exemple `F:\actions-runner`. À la question des étiquettes (labels), répondre `rhenium`. Refuser le service Windows : le runner doit tourner sous ton compte, qui a Visual Studio, Python et les outils.
8. Lancer `F:\actions-runner\run.cmd` et le laisser ouvert. Le runner apparaît « Idle » dans Settings, Actions, Runners.

## Publier une version

```powershell
git push origin main
git tag v0.5.0
git push origin v0.5.0
```

Le PC doit être allumé, le runner lancé. Avec un arbre déjà compilé, compter quelques minutes ; après une retouche d'un en-tête de Blink ou de V8, beaucoup plus.

## Précautions

- Un seul build à la fois : le workflow et un build lancé à la main se partagent `F:\Rhenium\chromium-next\src\out\content-arm`. Ne pas compiler à la main pendant une exécution.
- L'arbre doit correspondre aux correctifs commités : après une retouche dans l'arbre, `maj-chromium\regen-patches.ps1` puis commit. Sinon le build embarque les retouches non commitées, ou s'arrête sur un correctif qui ne s'applique plus.
- Le certificat ne doit pas changer : les téléphones qui ont installé une version refuseraient la suivante signée par un autre. `paquet.ps1` s'arrête s'il manque.
- Ne jamais ajouter le runner au dépôt public, ni rendre public le dépôt privé tant que le runner y est.

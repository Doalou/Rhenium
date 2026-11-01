// Copyright 2026 Doalo
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Rhénium : le moteur ouvre viewer.html?url=<PDF> à la place d'une réponse
// PDF. Le paramètre « file » de pdf.js refuse une autre origine que celle de
// la liseuse ; le PDF est donc ouvert ici, une fois la liseuse prête.
"use strict";

document.addEventListener("webviewerloaded", () => {
  const options = window.PDFViewerApplicationOptions;
  options.set("defaultUrl", "");
  options.set("disablePreferences", true);
  options.set("enableScripting", false);
  options.set("isEvalSupported", false);
  options.set("annotationEditorMode", -1);

  const url = new URLSearchParams(location.search).get("url");
  if (!url)
    return;
  const app = window.PDFViewerApplication;
  // Après la fin de run(), qui suit la même promesse.
  app.initializedPromise.then(() => setTimeout(() => {
    app.open({ url, originalUrl: url, withCredentials: true });
  }));
}, { once: true });

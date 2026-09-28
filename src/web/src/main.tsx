// SPDX-License-Identifier: AGPL-3.0-or-later
import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { App } from './app/App';
// Global styles: design tokens first, then the resets that consume them.
import './ui/tokens.css';
import './app/global.css';
// Initialises i18next before the first render (G-arch 1).
import './app/i18n';

const rootElement = document.getElementById('root');
if (!rootElement) {
  throw new Error('index.html must contain a #root element');
}

createRoot(rootElement).render(
  <StrictMode>
    <App />
  </StrictMode>,
);

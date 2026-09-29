// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * Saving the CSV that `sample/export` returns (TODO.md G3.2, F6.6).
 *
 * The RPC answers with a string body — the same chain-of-custody schema as
 * `freezerctl sample export` — so the only work here is naming the file and
 * handing it to the browser. Both are separated from the screen because they
 * are the parts a test can pin down: the name is a contract ("samples-<lab>-
 * <date>.csv"), and a blob URL that is never revoked leaks the whole export.
 */

/**
 * The date part of the file name, in UTC.
 *
 * UTC because every timestamp *inside* the CSV is UTC (G-arch 9 keeps the
 * browser's zone for display only), so a file named for "today" must not
 * disagree with the rows in it.
 */
export function exportDateStamp(date: Date = new Date()): string {
  return date.toISOString().slice(0, 10);
}

/**
 * `samples-<lab>-<date>.csv`.
 *
 * `<lab>` is the lab **id**, which the route already carries: it is stable,
 * unique, and cannot collide the way two labs called "Lab" would. It is
 * sanitised anyway — the id comes from the URL, and a name that ends up in a
 * `download` attribute must never contain a path separator.
 */
export function exportFileName(labId: string, date: Date = new Date()): string {
  const lab = labId.replaceAll(/[^A-Za-z0-9._-]/g, '-');
  return `samples-${lab === '' ? 'lab' : lab}-${exportDateStamp(date)}.csv`;
}

/**
 * Save a text body as a download.
 *
 * `createObjectURL` rather than a `data:` URL: a lab-wide export can be tens of
 * megabytes, and browsers cap `data:` URLs far below that.
 */
export function downloadTextFile(fileName: string, content: string, mimeType = 'text/csv'): void {
  const url = URL.createObjectURL(new Blob([content], { type: `${mimeType};charset=utf-8` }));
  const link = document.createElement('a');
  link.href = url;
  link.download = fileName;
  link.rel = 'noopener';
  document.body.append(link);
  link.click();
  link.remove();
  URL.revokeObjectURL(url);
}

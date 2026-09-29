// SPDX-License-Identifier: AGPL-3.0-or-later
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { downloadTextFile, exportDateStamp, exportFileName } from './exportSamples';

/**
 * CSV export (TODO.md G3.2, F6.6): `sample/export` answers with the CSV body,
 * and the browser saves it as `samples-<lab>-<date>.csv`.
 *
 * jsdom implements neither `URL.createObjectURL` nor a download, so the two
 * seams a browser provides are stubbed here; what is asserted is the part this
 * module owns — the file name and the bytes handed to the browser.
 */

const createObjectURL = vi.fn(() => 'blob:test');
const revokeObjectURL = vi.fn();

beforeEach(() => {
  // jsdom has no `URL.createObjectURL` at all, so this replaces a hole rather
  // than a working default.
  vi.stubGlobal(
    'URL',
    Object.assign(class extends URL {}, { createObjectURL, revokeObjectURL }),
  );
});

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
  createObjectURL.mockClear();
  revokeObjectURL.mockClear();
});

describe('exportFileName', () => {
  it('names the file samples-<lab>-<date>.csv', () => {
    expect(exportFileName('lab-demo', new Date('2026-10-02T12:34:56Z'))).toBe(
      'samples-lab-demo-2026-10-02.csv',
    );
  });

  it('uses UTC, like every timestamp in the file it names', () => {
    // 23:30 UTC on the 1st is "the 2nd" in most of Europe; the CSV's own
    // created_at values are UTC, so the file name must not disagree with them.
    expect(exportDateStamp(new Date('2026-10-01T23:30:00Z'))).toBe('2026-10-01');
  });

  it('keeps a path-hostile lab id from escaping the name', () => {
    const name = exportFileName('../../etc/passwd', new Date('2026-10-02T00:00:00Z'));

    expect(name).toBe('samples-..-..-etc-passwd-2026-10-02.csv');
    expect(name).not.toContain('/');
  });

  it('falls back to "lab" when there is no id, rather than producing samples--date.csv', () => {
    expect(exportFileName('', new Date('2026-10-02T00:00:00Z'))).toBe('samples-lab-2026-10-02.csv');
  });
});

describe('downloadTextFile', () => {
  it('hands the browser a CSV blob under the requested name', () => {
    const click = vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(() => {});
    const anchors: HTMLAnchorElement[] = [];
    click.mockImplementation(function (this: HTMLAnchorElement) {
      anchors.push(this);
    });

    downloadTextFile('samples-lab-demo-2026-10-02.csv', 'id,name\nsample-1,Serum A\n');

    expect(anchors).toHaveLength(1);
    expect(anchors[0]?.download).toBe('samples-lab-demo-2026-10-02.csv');
    expect(anchors[0]?.href).toBe('blob:test');
    const blob = createObjectURL.mock.calls[0]?.[0] as unknown as Blob;
    expect(blob.type).toBe('text/csv;charset=utf-8');
    // The object URL is released again, or the page leaks the whole export.
    expect(revokeObjectURL).toHaveBeenCalledWith('blob:test');
    // And the anchor does not stay in the document.
    expect(document.querySelector('a')).toBeNull();
  });

  it('writes the CSV body into the blob unchanged', async () => {
    vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(() => {});
    const csv = 'id,name\nsample-1,"Serum, A"\n';

    downloadTextFile('samples-lab-demo-2026-10-02.csv', csv);

    const blob = createObjectURL.mock.calls[0]?.[0] as unknown as Blob;
    await expect(blob.text()).resolves.toBe(csv);
  });
});

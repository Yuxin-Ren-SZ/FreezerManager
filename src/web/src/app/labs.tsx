// SPDX-License-Identifier: AGPL-3.0-or-later
import { createContext, useCallback, useContext, useMemo, useState } from 'react';
import type { ReactNode } from 'react';
import { useSession, type LabMembership } from './session';

/**
 * The selected lab id is a UI preference, which is the one category of thing
 * G-arch 7 allows in `localStorage`: no API payload ever goes in here.
 */
const STORAGE_KEY = 'fmgr.selectedLabId';

function readStoredLabId(): string | null {
  try {
    return globalThis.localStorage.getItem(STORAGE_KEY);
  } catch {
    // Safari in private mode throws rather than returning null. A missing
    // memory of the last lab is not worth failing a page load over.
    return null;
  }
}

function writeStoredLabId(labId: string): void {
  try {
    globalThis.localStorage.setItem(STORAGE_KEY, labId);
  } catch {
    // Same as above: the selection still applies for this session.
  }
}

export interface LabValue {
  /** Every lab the signed-in user is a member of. */
  labs: readonly LabMembership[];
  /** The selected lab, or `null` when the user belongs to none. */
  selectedLab: LabMembership | null;
  selectedLabId: string | null;
  selectLab: (labId: string) => void;
}

const LabContext = createContext<LabValue | null>(null);

export function LabProvider({ children }: { children: ReactNode }) {
  const { user } = useSession();
  const [preferredLabId, setPreferredLabId] = useState<string | null>(readStoredLabId);
  const labs = useMemo(() => user?.labs ?? [], [user]);

  // A remembered lab id can stop being valid — membership revoked, or the user
  // signed in as somebody else — so it is a preference, not the truth. Falling
  // back to the first membership keeps every lab-scoped link working.
  const selectedLabId = useMemo(() => {
    if (preferredLabId !== null && labs.some((lab) => lab.labId === preferredLabId)) {
      return preferredLabId;
    }
    return labs[0]?.labId ?? null;
  }, [preferredLabId, labs]);

  const selectLab = useCallback((labId: string) => {
    setPreferredLabId(labId);
    writeStoredLabId(labId);
  }, []);

  const value = useMemo<LabValue>(
    () => ({
      labs,
      selectedLabId,
      selectedLab: labs.find((lab) => lab.labId === selectedLabId) ?? null,
      selectLab,
    }),
    [labs, selectedLabId, selectLab],
  );

  return <LabContext.Provider value={value}>{children}</LabContext.Provider>;
}

export function useLabs(): LabValue {
  const value = useContext(LabContext);
  if (value === null) {
    throw new Error('useLabs() must be called inside a <LabProvider>');
  }
  return value;
}

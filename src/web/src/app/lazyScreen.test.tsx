// SPDX-License-Identifier: AGPL-3.0-or-later
import { Component, Suspense, type ReactNode } from 'react';
import { render, screen } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { lazyScreen } from './lazyScreen';

function GreetingScreen() {
  return <h1>Greeting screen</h1>;
}

/** Only an error boundary can observe a rejected `lazy()` load. */
class CatchRenderError extends Component<{ children: ReactNode }, { message: string | null }> {
  override state: { message: string | null } = { message: null };

  static getDerivedStateFromError(error: Error) {
    return { message: error.message };
  }

  override render() {
    return this.state.message === null ? (
      this.props.children
    ) : (
      <p role="alert">{this.state.message}</p>
    );
  }
}

describe('lazyScreen', () => {
  it('renders the named export once its module has loaded', async () => {
    const Screen = lazyScreen(() => Promise.resolve({ GreetingScreen }), 'GreetingScreen');

    render(
      <Suspense fallback={<p>waiting</p>}>
        <Screen />
      </Suspense>,
    );

    expect(await screen.findByRole('heading', { name: 'Greeting screen' })).toBeInTheDocument();
    expect(screen.queryByText('waiting')).not.toBeInTheDocument();
  });

  it('does not call the loader until the component first renders', async () => {
    const load = vi.fn(() => Promise.resolve({ GreetingScreen }));
    const Screen = lazyScreen(load, 'GreetingScreen');
    expect(load).not.toHaveBeenCalled();

    render(
      <Suspense fallback={<p>waiting</p>}>
        <Screen />
      </Suspense>,
    );

    expect(await screen.findByRole('heading', { name: 'Greeting screen' })).toBeInTheDocument();
    expect(load).toHaveBeenCalledTimes(1);
  });

  it('fails loudly when the module has no such export, instead of rendering nothing', async () => {
    // A typo in the export name is the failure this exists to catch: without
    // the check `lazy()` resolves to `undefined` and React throws a far less
    // specific error at render time.
    const consoleError = vi.spyOn(console, 'error').mockImplementation(() => undefined);
    // Typed as a plain record on purpose: a caller that does not know the
    // module's shape (which is what the type parameter normally supplies) gets
    // the runtime check rather than a compile error.
    const module: Record<string, unknown> = { GreetingScreen };
    const Screen = lazyScreen(() => Promise.resolve(module), 'RenamedScreen');

    render(
      <CatchRenderError>
        <Suspense fallback={<p>waiting</p>}>
          <Screen />
        </Suspense>
      </CatchRenderError>,
    );

    expect(await screen.findByRole('alert')).toHaveTextContent('RenamedScreen');
    consoleError.mockRestore();
  });
});

import { Injectable } from '@angular/core';
import { firstValueFrom, Observable, of } from 'rxjs';
import { catchError, map, tap } from 'rxjs/operators';
import { HttpsvcService } from './httpsvc.service';
import { SessionInfo } from './app-globals';

/// Holds the current auth session for the SPA.
/// Adapted from xpmile SessionService — same pattern: load once at
/// APP_INITIALIZER, cache, guard reads the cache.
@Injectable({ providedIn: 'root' })
export class SessionService {

  private session: SessionInfo | null = null;

  constructor(private http: HttpsvcService) {}

  get isAuthenticated(): boolean {
    return this.session !== null;
  }

  get role(): string | undefined {
    return this.session?.role;
  }

  clear(): void {
    this.session = null;
  }

  /// Try to get a session from the backend.  A 401 (no session cookie)
  /// resolves to null — not an error — so the guard can redirect to login.
  loadSession(): Observable<SessionInfo | null> {
    // Probe /api/v1/status — returns 401 when unauthenticated.
    return this.http.getStatus().pipe(
      map(() => {
        this.session = { role: 'admin' };
        return this.session;
      }),
      catchError(() => { this.session = null; return of(null); })
    );
  }

  /// Called after a successful login — cache the session so the guard
  /// doesn't need another round trip.
  setFromLogin(): void {
    this.session = { role: 'admin' };
  }
}

/// APP_INITIALIZER factory — loads the session once before the app
/// bootstraps, so a reload of an authenticated page isn't bounced to /login.
export function initSession(session: SessionService): () => Promise<unknown> {
  return () => firstValueFrom(session.loadSession());
}

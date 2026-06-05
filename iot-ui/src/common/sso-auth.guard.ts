import { Injectable } from '@angular/core';
import { CanActivate, Router, UrlTree } from '@angular/router';
import { map, Observable, of } from 'rxjs';
import { SessionService } from './session.service';

/// Route guard for authenticated areas.  Allows the route when a
/// session is already cached; otherwise loads the session (covering a
/// login that happened without a page reload) and either allows or
/// redirects to /login.
@Injectable({ providedIn: 'root' })
export class SsoAuthGuard implements CanActivate {

  constructor(private session: SessionService, private router: Router) {}

  canActivate(): Observable<boolean | UrlTree> {
    if (this.session.isAuthenticated) {
      return of(true);
    }
    return this.session.loadSession().pipe(
      map(s => s ? true : this.router.createUrlTree(['/login']))
    );
  }
}

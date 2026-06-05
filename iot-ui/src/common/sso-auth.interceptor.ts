import { Injectable } from '@angular/core';
import {
  HttpInterceptor, HttpRequest, HttpHandler,
  HttpEvent, HttpErrorResponse
} from '@angular/common/http';
import { Observable, throwError } from 'rxjs';
import { catchError } from 'rxjs/operators';
import { Router } from '@angular/router';
import { SessionService } from './session.service';

/// Catches 401 responses, clears the cached session, and redirects to
/// /login.  Non-401 errors pass through unchanged.
@Injectable()
export class SsoAuthInterceptor implements HttpInterceptor {

  constructor(private session: SessionService, private router: Router) {}

  intercept(req: HttpRequest<unknown>, next: HttpHandler):
      Observable<HttpEvent<unknown>> {
    return next.handle(req).pipe(
      catchError((err: HttpErrorResponse) => {
        if (err.status === 401) {
          this.session.clear();
          // Don't redirect if we're already on the login page
          if (!window.location.pathname.startsWith('/login')) {
            this.router.navigate(['/login']);
          }
        }
        return throwError(() => err);
      })
    );
  }
}

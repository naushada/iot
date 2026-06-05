import { Component } from '@angular/core';
import { Router } from '@angular/router';
import { HttpsvcService } from '../../common/httpsvc.service';
import { SessionService } from '../../common/session.service';

@Component({
  selector: 'app-login',
  templateUrl: './login.component.html',
  styleUrls: ['./login.component.scss']
})
export class LoginComponent {
  id = 'admin'; password = ''; error = ''; loggingIn = false;

  constructor(private http: HttpsvcService, private session: SessionService, private router: Router) {}

  onLogin(): void {
    if (!this.id || !this.password) { this.error = 'ID and password required'; return; }
    this.loggingIn = true; this.error = '';
    this.http.login({ id: this.id, password: this.password }).subscribe({
      next: (r) => {
        this.loggingIn = false;
        if (r.ok) { this.session.setFromLogin(r.access); this.router.navigate(['/main']); }
        else { this.error = r.err || 'Login failed'; }
      },
      error: () => { this.loggingIn = false; this.error = 'Connection failed'; }
    });
  }
}

import { Component, OnInit } from '@angular/core';
import { HttpsvcService } from '../../common/httpsvc.service';

@Component({
  selector: 'app-log-level',
  template: `
    <div class="log-level">
      <label>Log Level</label>
      <select class="clr-select" [(ngModel)]="level" (change)="save()">
        <option *ngFor="let l of levels" [value]="l">{{ l }}</option>
      </select>
      <span *ngIf="msg" [style.color]="msg==='Set to '+level?'#66bb6a':'#c62828'"
            style="margin-left:10px;font-size:12px;">{{ msg }}</span>
    </div>
  `,
  styles: [`
    .log-level { display: flex; align-items: center; gap: 10px; padding: 6px 1rem; }
    label { font-size: 12px; color: #9e9e9e; }
    .clr-select { background: #fff; border: 1px solid #ccc; color: #333;
      padding: 4px 8px; border-radius: 3px; font-size: 12px; cursor: pointer; }
    .clr-select:focus { outline: none; border-color: #2e7d32; }
  `]
})
export class LogLevelComponent implements OnInit {
  level = 'INFO';
  levels = ['DEBUG', 'INFO', 'WARNING', 'ERROR'];
  msg = '';

  constructor(private http: HttpsvcService) {}

  ngOnInit(): void {
    this.http.dbGet(['log.level']).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          const v = (r.data['log.level'] as string || 'INFO').toUpperCase();
          if (this.levels.includes(v)) this.level = v;
        }
      }
    });
  }

  save(): void {
    this.msg = '';
    this.http.dbSet([{ key: 'log.level', value: this.level }]).subscribe({
      next: (r) => {
        this.msg = r.ok ? 'Set to ' + this.level : 'Error: ' + r.err;
      },
      error: () => { this.msg = 'Save failed'; }
    });
  }
}

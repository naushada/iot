import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { DataStoreService } from '../../common/datastore.service';

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
    
    .clr-select:focus { outline: none; border-color: #2e7d32; }
  `]
})
export class LogLevelComponent implements OnInit, OnDestroy {
  level = 'INFO';
  levels = ['DEBUG', 'INFO', 'WARNING', 'ERROR'];
  msg = '';
  private sub = new Subscription();

  constructor(private ds: DataStoreService) {}

  ngOnInit(): void {
    // Paint from the shared prefetched cache, then stay live off the appglobal
    // store. log.level is written here and re-seeded into the cache on save.
    this.applyLevel(this.ds.getString('log.level', 'INFO'));
    this.sub.add(this.ds.observe('log.level')
      .subscribe(v => this.applyLevel(typeof v === 'string' ? v : 'INFO')));
  }

  ngOnDestroy(): void { this.sub.unsubscribe(); }

  private applyLevel(v: string): void {
    const up = (v || 'INFO').toUpperCase();
    if (this.levels.includes(up)) this.level = up;
  }

  save(): void {
    this.msg = '';
    this.ds.write([{ key: 'log.level', value: this.level }]).subscribe({
      next: (r) => {
        this.msg = r.ok ? 'Set to ' + this.level : 'Error: ' + r.err;
      },
      error: () => { this.msg = 'Save failed'; }
    });
  }
}

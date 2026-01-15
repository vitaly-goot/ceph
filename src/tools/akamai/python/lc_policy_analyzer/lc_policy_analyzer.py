#!/usr/bin/env python3
"""
RGW Lifecycle Policy Analyzer and Optimizer

This tool analyzes S3/RGW lifecycle policies to identify inefficiencies and
recommend optimizations that reduce LC processing overhead.

Usage:
    ./lc_policy_analyzer.py --file policy.json
    ./lc_policy_analyzer.py --file policy.xml --format xml
    ./lc_policy_analyzer.py --bucket my-bucket --profile prod
    cat policy.json | ./lc_policy_analyzer.py --stdin

"""

import argparse
import json
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict
from dataclasses import dataclass, field
from typing import List, Dict, Set, Optional, Tuple
from datetime import datetime
import re


@dataclass
class LCRule:
    """Represents a single lifecycle rule"""
    rule_id: str
    status: str
    prefix: str = ""
    tags: Dict[str, str] = field(default_factory=dict)
    
    # Actions
    expiration_days: Optional[int] = None
    expiration_date: Optional[str] = None
    noncurrent_expiration_days: Optional[int] = None
    transitions: List[Tuple[int, str]] = field(default_factory=list)
    noncurrent_transitions: List[Tuple[int, str]] = field(default_factory=list)
    abort_incomplete_mpu_days: Optional[int] = None
    expired_delete_marker: bool = False
    
    # Metadata
    has_size_filter: bool = False
    size_greater_than: Optional[int] = None
    size_less_than: Optional[int] = None
    
    def __hash__(self):
        return hash(self.rule_id)


@dataclass
class Issue:
    """Represents an optimization issue"""
    severity: str  # CRITICAL, WARNING, INFO
    category: str
    rule_ids: List[str]
    description: str
    impact: str
    recommendation: str
    estimated_savings: str = ""


class LCPolicyAnalyzer:
    """Analyzes lifecycle policies for optimization opportunities"""
    
    def __init__(self):
        self.rules: List[LCRule] = []
        self.issues: List[Issue] = []
        self.prefix_map: Dict[str, List[LCRule]] = defaultdict(list)
        self.tag_rules: List[LCRule] = []
        
    def load_from_json(self, json_data: dict):
        """Load policy from JSON format (AWS S3 API format)"""
        rules_data = json_data.get('Rules', [])
        
        for rule_data in rules_data:
            rule = self._parse_json_rule(rule_data)
            self.rules.append(rule)
            
            # Build indices
            self.prefix_map[rule.prefix].append(rule)
            if rule.tags:
                self.tag_rules.append(rule)
    
    def _parse_json_rule(self, rule_data: dict) -> LCRule:
        """Parse a single rule from JSON"""
        rule = LCRule(
            rule_id=rule_data.get('ID', 'unknown'),
            status=rule_data.get('Status', 'Disabled')
        )
        
        # Parse filter
        filter_data = rule_data.get('Filter', {})
        if 'Prefix' in filter_data:
            rule.prefix = filter_data['Prefix']
        elif 'Prefix' in rule_data:
            rule.prefix = rule_data['Prefix']
            
        if 'And' in filter_data:
            and_filter = filter_data['And']
            if 'Prefix' in and_filter:
                rule.prefix = and_filter['Prefix']
            if 'Tags' in and_filter:
                for tag in and_filter['Tags']:
                    rule.tags[tag['Key']] = tag['Value']
        elif 'Tag' in filter_data:
            tag = filter_data['Tag']
            rule.tags[tag['Key']] = tag['Value']
            
        # Size filters
        if 'ObjectSizeGreaterThan' in filter_data:
            rule.has_size_filter = True
            rule.size_greater_than = filter_data['ObjectSizeGreaterThan']
        if 'ObjectSizeLessThan' in filter_data:
            rule.has_size_filter = True
            rule.size_less_than = filter_data['ObjectSizeLessThan']
            
        # Parse actions
        if 'Expiration' in rule_data:
            exp = rule_data['Expiration']
            rule.expiration_days = exp.get('Days')
            rule.expiration_date = exp.get('Date')
            rule.expired_delete_marker = exp.get('ExpiredObjectDeleteMarker', False)
            
        if 'NoncurrentVersionExpiration' in rule_data:
            rule.noncurrent_expiration_days = rule_data['NoncurrentVersionExpiration'].get('NoncurrentDays')
            
        if 'Transitions' in rule_data:
            for trans in rule_data['Transitions']:
                days = trans.get('Days')
                storage = trans.get('StorageClass')
                if days and storage:
                    rule.transitions.append((days, storage))
                    
        if 'NoncurrentVersionTransitions' in rule_data:
            for trans in rule_data['NoncurrentVersionTransitions']:
                days = trans.get('NoncurrentDays')
                storage = trans.get('StorageClass')
                if days and storage:
                    rule.noncurrent_transitions.append((days, storage))
                    
        if 'AbortIncompleteMultipartUpload' in rule_data:
            rule.abort_incomplete_mpu_days = rule_data['AbortIncompleteMultipartUpload'].get('DaysAfterInitiation')
            
        return rule
    
    def load_from_xml(self, xml_string: str):
        """Load policy from XML format"""
        root = ET.fromstring(xml_string)
        
        for rule_elem in root.findall('Rule'):
            rule = self._parse_xml_rule(rule_elem)
            self.rules.append(rule)
            
            # Build indices
            self.prefix_map[rule.prefix].append(rule)
            if rule.tags:
                self.tag_rules.append(rule)
    
    def _parse_xml_rule(self, rule_elem: ET.Element) -> LCRule:
        """Parse a single rule from XML"""
        rule = LCRule(
            rule_id=rule_elem.findtext('ID', 'unknown'),
            status=rule_elem.findtext('Status', 'Disabled')
        )
        
        # Parse filter
        filter_elem = rule_elem.find('Filter')
        if filter_elem is not None:
            prefix_elem = filter_elem.find('Prefix')
            if prefix_elem is not None:
                rule.prefix = prefix_elem.text or ""
                
            and_elem = filter_elem.find('And')
            if and_elem is not None:
                prefix_elem = and_elem.find('Prefix')
                if prefix_elem is not None:
                    rule.prefix = prefix_elem.text or ""
                for tag_elem in and_elem.findall('Tag'):
                    key = tag_elem.findtext('Key', '')
                    value = tag_elem.findtext('Value', '')
                    rule.tags[key] = value
            
            tag_elem = filter_elem.find('Tag')
            if tag_elem is not None:
                key = tag_elem.findtext('Key', '')
                value = tag_elem.findtext('Value', '')
                rule.tags[key] = value
                
            # Size filters
            size_gt = filter_elem.find('ObjectSizeGreaterThan')
            if size_gt is not None:
                rule.has_size_filter = True
                rule.size_greater_than = int(size_gt.text)
            size_lt = filter_elem.find('ObjectSizeLessThan')
            if size_lt is not None:
                rule.has_size_filter = True
                rule.size_less_than = int(size_lt.text)
        else:
            # Legacy prefix
            prefix_elem = rule_elem.find('Prefix')
            if prefix_elem is not None:
                rule.prefix = prefix_elem.text or ""
        
        # Parse actions
        exp_elem = rule_elem.find('Expiration')
        if exp_elem is not None:
            days_elem = exp_elem.find('Days')
            if days_elem is not None:
                rule.expiration_days = int(days_elem.text)
            date_elem = exp_elem.find('Date')
            if date_elem is not None:
                rule.expiration_date = date_elem.text
            edm_elem = exp_elem.find('ExpiredObjectDeleteMarker')
            if edm_elem is not None:
                rule.expired_delete_marker = edm_elem.text.lower() == 'true'
                
        noncur_exp_elem = rule_elem.find('NoncurrentVersionExpiration')
        if noncur_exp_elem is not None:
            days_elem = noncur_exp_elem.find('NoncurrentDays')
            if days_elem is not None:
                rule.noncurrent_expiration_days = int(days_elem.text)
                
        for trans_elem in rule_elem.findall('Transition'):
            days_elem = trans_elem.find('Days')
            storage_elem = trans_elem.find('StorageClass')
            if days_elem is not None and storage_elem is not None:
                rule.transitions.append((int(days_elem.text), storage_elem.text))
                
        for trans_elem in rule_elem.findall('NoncurrentVersionTransition'):
            days_elem = trans_elem.find('NoncurrentDays')
            storage_elem = trans_elem.find('StorageClass')
            if days_elem is not None and storage_elem is not None:
                rule.noncurrent_transitions.append((int(days_elem.text), storage_elem.text))
                
        mpu_elem = rule_elem.find('AbortIncompleteMultipartUpload')
        if mpu_elem is not None:
            days_elem = mpu_elem.find('DaysAfterInitiation')
            if days_elem is not None:
                rule.abort_incomplete_mpu_days = int(days_elem.text)
                
        return rule
    
    def analyze(self):
        """Run all analysis checks"""
        self._check_overlapping_prefixes()
        self._check_duplicate_prefixes()
        self._check_redundant_rules()
        self._check_tag_filter_efficiency()
        self._check_action_ordering()
        self._check_disabled_rules()
        self._check_empty_actions()
        self._check_prefix_efficiency()
        self._check_multiple_tag_reads()
        self._check_mpu_cleanup()
        self._check_size_filters()
        
    def _check_overlapping_prefixes(self):
        """Check for rules with overlapping prefixes"""
        prefixes = sorted(self.prefix_map.keys(), key=len, reverse=True)
        
        overlaps = []
        for i, prefix1 in enumerate(prefixes):
            for prefix2 in prefixes[i+1:]:
                if prefix1.startswith(prefix2):
                    rules1 = [r.rule_id for r in self.prefix_map[prefix1] if r.status == 'Enabled']
                    rules2 = [r.rule_id for r in self.prefix_map[prefix2] if r.status == 'Enabled']
                    if rules1 and rules2:
                        overlaps.append((prefix1, rules1, prefix2, rules2))
        
        if overlaps:
            for prefix1, rules1, prefix2, rules2 in overlaps:
                all_rules = rules1 + rules2
                self.issues.append(Issue(
                    severity="WARNING",
                    category="Prefix Overlap",
                    rule_ids=all_rules,
                    description=f"Prefix '{prefix1}' (rules: {', '.join(rules1)}) overlaps with '{prefix2}' (rules: {', '.join(rules2)})",
                    impact="Objects matching longer prefix will ONLY be processed by longer-prefix rule. Shorter prefix rule won't process these objects.",
                    recommendation=f"Verify this is intentional. If you want both rules to apply, use different prefixes. RGW processes objects with longest matching prefix only.",
                    estimated_savings="Avoids confusion about which rule applies"
                ))
    
    def _check_duplicate_prefixes(self):
        """Check for multiple rules with same prefix"""
        for prefix, rules in self.prefix_map.items():
            enabled_rules = [r for r in rules if r.status == 'Enabled']
            
            if len(enabled_rules) > 1:
                # Check if they're distinguished by tags
                tag_distinguished = all(r.tags for r in enabled_rules)
                
                if not tag_distinguished:
                    self.issues.append(Issue(
                        severity="CRITICAL",
                        category="Duplicate Prefix",
                        rule_ids=[r.rule_id for r in enabled_rules],
                        description=f"Multiple rules with same prefix '{prefix}': {', '.join(r.rule_id for r in enabled_rules)}",
                        impact="⚠️ AMBIGUOUS BEHAVIOR! RGW prefix_map may use insertion order or undefined behavior. Only one rule will actually execute per object.",
                        recommendation=f"Consolidate these rules into ONE rule with multiple actions, OR use different prefixes, OR distinguish by tags.",
                        estimated_savings="Eliminates undefined behavior and wasted rule evaluations"
                    ))
                else:
                    # Tag-distinguished rules on same prefix
                    self.issues.append(Issue(
                        severity="WARNING",
                        category="Tag Filtering",
                        rule_ids=[r.rule_id for r in enabled_rules],
                        description=f"Multiple rules on prefix '{prefix}' distinguished by tags",
                        impact=f"⚠️ LC will read object tags {len(enabled_rules)} times per object (once per rule) unless tag caching is enabled.",
                        recommendation="Consider consolidating rules OR ensure your RGW version has tag caching optimization. Each tag read = 1-2ms RADOS overhead.",
                        estimated_savings=f"With tag caching: save {len(enabled_rules)-1}× tag reads = {(len(enabled_rules)-1)*2}ms per object"
                    ))
    
    def _check_redundant_rules(self):
        """Check for rules that will never execute"""
        for prefix, rules in self.prefix_map.items():
            enabled_rules = [r for r in rules if r.status == 'Enabled']
            
            if len(enabled_rules) > 1 and not any(r.tags for r in enabled_rules):
                # Multiple rules with same prefix, no tag distinction
                sorted_rules = sorted(
                    enabled_rules,
                    key=lambda r: (
                        r.expiration_days or 99999,
                        r.transitions[0][0] if r.transitions else 99999
                    )
                )
                
                if len(sorted_rules) >= 2:
                    earliest = sorted_rules[0]
                    later_rules = sorted_rules[1:]
                    
                    self.issues.append(Issue(
                        severity="WARNING",
                        category="Redundant Rules",
                        rule_ids=[r.rule_id for r in later_rules],
                        description=f"Rules {', '.join(r.rule_id for r in later_rules)} may never execute",
                        impact=f"Rule '{earliest.rule_id}' will process objects first (earliest action). Later rules likely won't see these objects.",
                        recommendation="Remove redundant rules or consolidate into single rule with multiple actions.",
                        estimated_savings="Reduces rule evaluation overhead"
                    ))
    
    def _check_tag_filter_efficiency(self):
        """Check for inefficient tag filtering"""
        if not self.tag_rules:
            return
        
        # Group by prefix
        prefix_tag_rules = defaultdict(list)
        for rule in self.tag_rules:
            if rule.status == 'Enabled':
                prefix_tag_rules[rule.prefix].append(rule)
        
        for prefix, rules in prefix_tag_rules.items():
            if len(rules) > 1:
                # Check if rules use same tags
                all_tags = set()
                for rule in rules:
                    all_tags.update(rule.tags.keys())
                
                # Estimate tag read overhead
                objects_per_prefix = "N"  # Unknown without bucket stats
                tag_reads_per_object = len(rules)
                overhead_ms = tag_reads_per_object * 2  # 2ms per tag read
                
                self.issues.append(Issue(
                    severity="WARNING",
                    category="Tag Filter Overhead",
                    rule_ids=[r.rule_id for r in rules],
                    description=f"{len(rules)} rules with tag filters on prefix '{prefix}'",
                    impact=f"Each object requires {tag_reads_per_object} tag reads (~{overhead_ms}ms overhead per object). For 1M objects: {overhead_ms * 1000 / 1000 / 60:.1f} minutes of tag read time.",
                    recommendation="Options:\n"
                                 "  1. Use tag caching (if available in your RGW version)\n"
                                 "  2. Consolidate rules if they check same tags\n"
                                 "  3. Use prefix-based organization instead of tags\n"
                                 "  4. Enable rgw_lc_wp_worker_max_aio > 1 to parallelize",
                    estimated_savings=f"Tag caching: save {(tag_reads_per_object-1)*2}ms per object = {(tag_reads_per_object-1)*33:.0f}% speedup"
                ))
    
    def _check_action_ordering(self):
        """Check for illogical action ordering"""
        for rule in self.rules:
            if rule.status != 'Enabled':
                continue
                
            issues = []
            
            # Check expiration vs transition ordering
            if rule.expiration_days and rule.transitions:
                earliest_transition = min(rule.transitions, key=lambda x: x[0])
                if rule.expiration_days < earliest_transition[0]:
                    issues.append(
                        f"Expiration ({rule.expiration_days}d) before first transition ({earliest_transition[0]}d). "
                        f"Transitions will never execute!"
                    )
            
            # Check transition ordering
            if len(rule.transitions) > 1:
                sorted_trans = sorted(rule.transitions, key=lambda x: x[0])
                if sorted_trans != rule.transitions:
                    issues.append(
                        f"Transitions not in chronological order. RGW will use latest applicable, but this is confusing."
                    )
                
                # Check for same storage class
                storage_classes = [t[1] for t in rule.transitions]
                if len(storage_classes) != len(set(storage_classes)):
                    issues.append(
                        f"Multiple transitions to same storage class. Only the latest will execute."
                    )
            
            # Check noncurrent version actions
            if rule.noncurrent_expiration_days and rule.noncurrent_transitions:
                earliest_nc_transition = min(rule.noncurrent_transitions, key=lambda x: x[0])
                if rule.noncurrent_expiration_days < earliest_nc_transition[0]:
                    issues.append(
                        f"Noncurrent expiration ({rule.noncurrent_expiration_days}d) before noncurrent transition ({earliest_nc_transition[0]}d). "
                        f"Noncurrent transitions will never execute!"
                    )
            
            if issues:
                self.issues.append(Issue(
                    severity="CRITICAL",
                    category="Action Ordering",
                    rule_ids=[rule.rule_id],
                    description=f"Illogical action ordering in rule '{rule.rule_id}'",
                    impact="\n".join(f"  • {issue}" for issue in issues),
                    recommendation="Reorder actions chronologically: transitions first (ascending days), then expiration last.",
                    estimated_savings="Prevents wasted transitions and ensures expected behavior"
                ))
    
    def _check_disabled_rules(self):
        """Check for disabled rules"""
        disabled = [r for r in self.rules if r.status == 'Disabled']
        
        if disabled:
            self.issues.append(Issue(
                severity="INFO",
                category="Disabled Rules",
                rule_ids=[r.rule_id for r in disabled],
                description=f"{len(disabled)} disabled rules present",
                impact="These rules won't execute, but RGW still parses them.",
                recommendation="Remove disabled rules to reduce policy size and parsing overhead.",
                estimated_savings=f"Minor: ~{len(disabled) * 100} bytes policy size reduction"
            ))
    
    def _check_empty_actions(self):
        """Check for rules with no actions"""
        for rule in self.rules:
            if rule.status != 'Enabled':
                continue
                
            has_action = (
                rule.expiration_days or
                rule.expiration_date or
                rule.noncurrent_expiration_days or
                rule.transitions or
                rule.noncurrent_transitions or
                rule.abort_incomplete_mpu_days or
                rule.expired_delete_marker
            )
            
            if not has_action:
                self.issues.append(Issue(
                    severity="WARNING",
                    category="Empty Actions",
                    rule_ids=[rule.rule_id],
                    description=f"Rule '{rule.rule_id}' has no actions",
                    impact="Rule will list objects but take no action. Wastes LC processing time.",
                    recommendation="Add actions to the rule or disable/remove it.",
                    estimated_savings="Eliminates useless bucket listing and rule evaluation"
                ))
    
    def _check_prefix_efficiency(self):
        """Check for prefix patterns that cause excessive listing"""
        empty_prefix_rules = self.prefix_map.get("", [])
        
        if empty_prefix_rules:
            enabled_empty = [r for r in empty_prefix_rules if r.status == 'Enabled']
            if enabled_empty:
                total_rules = len([r for r in self.rules if r.status == 'Enabled'])
                
                self.issues.append(Issue(
                    severity="WARNING",
                    category="Broad Prefix",
                    rule_ids=[r.rule_id for r in enabled_empty],
                    description=f"Rules with empty prefix (matches ALL objects)",
                    impact=f"LC will list ENTIRE bucket ({total_rules} rule evaluations per object). "
                           f"For 1M objects: {total_rules}M rule evaluations!",
                    recommendation="Use specific prefixes to partition bucket:\n"
                                 "  • data/ → expire after 365 days\n"
                                 "  • logs/ → expire after 30 days\n"
                                 "  • temp/ → expire after 7 days\n"
                                 "This allows LC to skip irrelevant objects.",
                    estimated_savings=f"With prefixes: can skip {100 - (100/total_rules):.0f}% of objects per rule"
                ))
        
        # Check for very generic prefixes
        generic_prefixes = [p for p in self.prefix_map.keys() if len(p) <= 2 and p != ""]
        if generic_prefixes:
            rules = []
            for prefix in generic_prefixes:
                rules.extend([r.rule_id for r in self.prefix_map[prefix] if r.status == 'Enabled'])
            
            if rules:
                self.issues.append(Issue(
                    severity="INFO",
                    category="Generic Prefix",
                    rule_ids=rules,
                    description=f"Very short prefixes: {', '.join(repr(p) for p in generic_prefixes)}",
                    impact="Short prefixes match many objects. Consider more specific prefixes.",
                    recommendation="Use hierarchical prefixes for better partitioning:\n"
                                 "  • Instead of 'a' → use 'archive/2024/'\n"
                                 "  • Instead of 'l' → use 'logs/application/'",
                    estimated_savings="Better prefix selectivity reduces objects processed per rule"
                ))
    
    def _check_multiple_tag_reads(self):
        """Check if same tags are checked multiple times"""
        if len(self.tag_rules) < 2:
            return
        
        # Group by prefix and check tag overlap
        for prefix, rules in self.prefix_map.items():
            tag_rules_this_prefix = [r for r in rules if r.tags and r.status == 'Enabled']
            
            if len(tag_rules_this_prefix) >= 2:
                # Check tag key overlap
                tag_keys_per_rule = [set(r.tags.keys()) for r in tag_rules_this_prefix]
                all_tags = set.union(*tag_keys_per_rule) if tag_keys_per_rule else set()
                
                overlapping_tags = set()
                for i, tags1 in enumerate(tag_keys_per_rule):
                    for tags2 in tag_keys_per_rule[i+1:]:
                        overlapping_tags.update(tags1 & tags2)
                
                if overlapping_tags:
                    self.issues.append(Issue(
                        severity="WARNING",
                        category="Duplicate Tag Reads",
                        rule_ids=[r.rule_id for r in tag_rules_this_prefix],
                        description=f"Rules check overlapping tags: {', '.join(overlapping_tags)}",
                        impact=f"Without tag caching, each rule reads tags independently. "
                               f"{len(tag_rules_this_prefix)} rules × 2ms = {len(tag_rules_this_prefix)*2}ms per object.",
                        recommendation="Optimization options:\n"
                                     "  1. BEST: Ensure tag caching is enabled in RGW (pre-load tags once per object)\n"
                                     "  2. Consolidate rules that check same tags\n"
                                     "  3. Use prefix-based rules instead of tag-based",
                        estimated_savings=f"Tag caching: {(len(tag_rules_this_prefix)-1)*2}ms per object = {(len(tag_rules_this_prefix)-1)*33:.0f}% speedup"
                    ))
    
    def _check_mpu_cleanup(self):
        """Check for MPU cleanup rules"""
        mpu_rules = [r for r in self.rules if r.abort_incomplete_mpu_days and r.status == 'Enabled']
        
        if not mpu_rules:
            self.issues.append(Issue(
                severity="INFO",
                category="MPU Cleanup",
                rule_ids=[],
                description="No multipart upload cleanup rules found",
                impact="Incomplete MPUs may accumulate, wasting storage space.",
                recommendation="Add a rule to abort incomplete MPUs:\n"
                             "{\n"
                             "  'ID': 'abort-incomplete-mpu',\n"
                             "  'Status': 'Enabled',\n"
                             "  'Prefix': '',\n"
                             "  'AbortIncompleteMultipartUpload': {'DaysAfterInitiation': 7}\n"
                             "}",
                estimated_savings="Reclaim storage from abandoned uploads"
            ))
        else:
            # Check if MPU rules are efficient
            for rule in mpu_rules:
                if rule.prefix == "" and not rule.tags:
                    # Good: broad MPU cleanup
                    pass
                else:
                    self.issues.append(Issue(
                        severity="INFO",
                        category="MPU Cleanup",
                        rule_ids=[rule.rule_id],
                        description=f"MPU cleanup rule has specific prefix/tags: '{rule.prefix}'",
                        impact="MPU cleanup only applies to subset of bucket. Other incomplete MPUs may remain.",
                        recommendation="Consider a separate rule with empty prefix for comprehensive MPU cleanup.",
                        estimated_savings="Ensures all incomplete MPUs are cleaned up"
                    ))
    
    def _check_size_filters(self):
        """Check for size filter usage"""
        size_filter_rules = [r for r in self.rules if r.has_size_filter and r.status == 'Enabled']
        
        if size_filter_rules:
            self.issues.append(Issue(
                severity="INFO",
                category="Size Filters",
                rule_ids=[r.rule_id for r in size_filter_rules],
                description=f"{len(size_filter_rules)} rules use size filters",
                impact="Size filters require reading object metadata (already done during listing). No extra overhead.",
                recommendation="Size filters are efficient when used with broad prefixes. "
                             "Good for separating small vs large objects.",
                estimated_savings="N/A - size filtering is efficient"
            ))
    
    def generate_report(self, format='text') -> str:
        """Generate analysis report"""
        if format == 'text':
            return self._generate_text_report()
        elif format == 'json':
            return self._generate_json_report()
        elif format == 'markdown':
            return self._generate_markdown_report()
        else:
            raise ValueError(f"Unknown format: {format}")
    
    def _generate_text_report(self) -> str:
        """Generate text report"""
        lines = []
        lines.append("=" * 80)
        lines.append("RGW LIFECYCLE POLICY ANALYSIS REPORT")
        lines.append("=" * 80)
        lines.append(f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        lines.append(f"Total rules: {len(self.rules)}")
        lines.append(f"Enabled rules: {len([r for r in self.rules if r.status == 'Enabled'])}")
        lines.append(f"Disabled rules: {len([r for r in self.rules if r.status == 'Disabled'])}")
        lines.append(f"Rules with tag filters: {len(self.tag_rules)}")
        lines.append("")
        
        # Summary
        if self.issues:
            critical = len([i for i in self.issues if i.severity == 'CRITICAL'])
            warning = len([i for i in self.issues if i.severity == 'WARNING'])
            info = len([i for i in self.issues if i.severity == 'INFO'])
            
            lines.append("SUMMARY")
            lines.append("-" * 80)
            lines.append(f"  🔴 CRITICAL issues: {critical}")
            lines.append(f"  🟡 WARNING issues: {warning}")
            lines.append(f"  ℹ️  INFO issues: {info}")
            lines.append("")
        else:
            lines.append("✅ No issues found! Policy is well-optimized.")
            lines.append("")
        
        # Issues
        if self.issues:
            lines.append("DETAILED FINDINGS")
            lines.append("=" * 80)
            lines.append("")
            
            # Group by severity
            for severity in ['CRITICAL', 'WARNING', 'INFO']:
                severity_issues = [i for i in self.issues if i.severity == severity]
                if not severity_issues:
                    continue
                
                icon = {'CRITICAL': '🔴', 'WARNING': '🟡', 'INFO': 'ℹ️'}[severity]
                lines.append(f"{icon} {severity} ISSUES ({len(severity_issues)})")
                lines.append("-" * 80)
                lines.append("")
                
                for idx, issue in enumerate(severity_issues, 1):
                    lines.append(f"{idx}. [{issue.category}] {issue.description}")
                    lines.append(f"   Rules: {', '.join(issue.rule_ids) if issue.rule_ids else 'N/A'}")
                    lines.append(f"")
                    lines.append(f"   IMPACT:")
                    for line in issue.impact.split('\n'):
                        lines.append(f"     {line}")
                    lines.append(f"")
                    lines.append(f"   RECOMMENDATION:")
                    for line in issue.recommendation.split('\n'):
                        lines.append(f"     {line}")
                    if issue.estimated_savings:
                        lines.append(f"")
                        lines.append(f"   ESTIMATED SAVINGS: {issue.estimated_savings}")
                    lines.append("")
                    lines.append("-" * 80)
                    lines.append("")
        
        # Recommendations summary
        lines.append("")
        lines.append("TOP RECOMMENDATIONS")
        lines.append("=" * 80)
        
        critical_issues = [i for i in self.issues if i.severity == 'CRITICAL']
        if critical_issues:
            lines.append("🔴 CRITICAL - Fix immediately:")
            for issue in critical_issues:
                lines.append(f"  • {issue.description}")
        
        warning_issues = [i for i in self.issues if i.severity == 'WARNING']
        if warning_issues:
            lines.append("")
            lines.append("🟡 WARNING - High impact optimizations:")
            for issue in warning_issues[:3]:  # Top 3
                lines.append(f"  • {issue.description}")
        
        lines.append("")
        lines.append("=" * 80)
        lines.append("END OF REPORT")
        lines.append("=" * 80)
        
        return '\n'.join(lines)
    
    def _generate_json_report(self) -> str:
        """Generate JSON report"""
        report = {
            'generated': datetime.now().isoformat(),
            'summary': {
                'total_rules': len(self.rules),
                'enabled_rules': len([r for r in self.rules if r.status == 'Enabled']),
                'disabled_rules': len([r for r in self.rules if r.status == 'Disabled']),
                'tag_filter_rules': len(self.tag_rules),
                'issues': {
                    'critical': len([i for i in self.issues if i.severity == 'CRITICAL']),
                    'warning': len([i for i in self.issues if i.severity == 'WARNING']),
                    'info': len([i for i in self.issues if i.severity == 'INFO'])
                }
            },
            'issues': [
                {
                    'severity': issue.severity,
                    'category': issue.category,
                    'rule_ids': issue.rule_ids,
                    'description': issue.description,
                    'impact': issue.impact,
                    'recommendation': issue.recommendation,
                    'estimated_savings': issue.estimated_savings
                }
                for issue in self.issues
            ]
        }
        return json.dumps(report, indent=2)
    
    def _generate_markdown_report(self) -> str:
        """Generate Markdown report"""
        lines = []
        lines.append("# RGW Lifecycle Policy Analysis Report")
        lines.append("")
        lines.append(f"**Generated:** {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        lines.append("")
        
        # Summary
        lines.append("## Summary")
        lines.append("")
        lines.append(f"- **Total rules:** {len(self.rules)}")
        lines.append(f"- **Enabled rules:** {len([r for r in self.rules if r.status == 'Enabled'])}")
        lines.append(f"- **Disabled rules:** {len([r for r in self.rules if r.status == 'Disabled'])}")
        lines.append(f"- **Rules with tag filters:** {len(self.tag_rules)}")
        lines.append("")
        
        if self.issues:
            critical = len([i for i in self.issues if i.severity == 'CRITICAL'])
            warning = len([i for i in self.issues if i.severity == 'WARNING'])
            info = len([i for i in self.issues if i.severity == 'INFO'])
            
            lines.append("### Issues Found")
            lines.append("")
            lines.append(f"- 🔴 **CRITICAL:** {critical}")
            lines.append(f"- 🟡 **WARNING:** {warning}")
            lines.append(f"- ℹ️ **INFO:** {info}")
        else:
            lines.append("✅ **No issues found!** Policy is well-optimized.")
        
        lines.append("")
        
        # Issues by severity
        if self.issues:
            for severity in ['CRITICAL', 'WARNING', 'INFO']:
                severity_issues = [i for i in self.issues if i.severity == severity]
                if not severity_issues:
                    continue
                
                icon = {'CRITICAL': '🔴', 'WARNING': '🟡', 'INFO': 'ℹ️'}[severity]
                lines.append(f"## {icon} {severity} Issues")
                lines.append("")
                
                for issue in severity_issues:
                    lines.append(f"### [{issue.category}] {issue.description}")
                    lines.append("")
                    if issue.rule_ids:
                        lines.append(f"**Rules:** `{', '.join(issue.rule_ids)}`")
                        lines.append("")
                    lines.append(f"**Impact:**")
                    lines.append("")
                    lines.append(issue.impact)
                    lines.append("")
                    lines.append(f"**Recommendation:**")
                    lines.append("")
                    lines.append(issue.recommendation)
                    if issue.estimated_savings:
                        lines.append("")
                        lines.append(f"**Estimated Savings:** {issue.estimated_savings}")
                    lines.append("")
                    lines.append("---")
                    lines.append("")
        
        return '\n'.join(lines)
    
    def generate_optimized_policy(self) -> dict:
        """Generate an optimized version of the policy"""
        # TODO: Implement policy rewriting
        # This would automatically fix issues and generate optimized JSON
        pass


def main():
    parser = argparse.ArgumentParser(
        description='Analyze and optimize RGW lifecycle policies',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Analyze from JSON file
  %(prog)s --file policy.json
  
  # Analyze from XML file
  %(prog)s --file policy.xml --format xml
  
  # Analyze from stdin
  aws s3api get-bucket-lifecycle-configuration --bucket my-bucket | %(prog)s --stdin
  
  # Output as JSON
  %(prog)s --file policy.json --output json
  
  # Output as Markdown
  %(prog)s --file policy.json --output markdown > report.md
  
  # Fetch from S3 and analyze
  %(prog)s --bucket my-bucket --profile production
        """
    )
    
    input_group = parser.add_mutually_exclusive_group(required=True)
    input_group.add_argument('--file', help='Path to lifecycle policy file (JSON or XML)')
    input_group.add_argument('--stdin', action='store_true', help='Read policy from stdin')
    input_group.add_argument('--bucket', help='Fetch policy from S3 bucket')
    
    parser.add_argument('--format', choices=['json', 'xml'], default='json',
                       help='Input format (default: json)')
    parser.add_argument('--output', choices=['text', 'json', 'markdown'], default='text',
                       help='Output format (default: text)')
    parser.add_argument('--profile', help='AWS profile name (for --bucket)')
    parser.add_argument('--endpoint-url', help='S3 endpoint URL (for --bucket)')
    
    args = parser.parse_args()
    
    analyzer = LCPolicyAnalyzer()
    
    # Load policy
    try:
        if args.stdin:
            content = sys.stdin.read()
            if args.format == 'json':
                policy_data = json.loads(content)
                analyzer.load_from_json(policy_data)
            else:
                analyzer.load_from_xml(content)
        
        elif args.file:
            with open(args.file, 'r') as f:
                content = f.read()
            
            if args.format == 'json' or args.file.endswith('.json'):
                policy_data = json.loads(content)
                analyzer.load_from_json(policy_data)
            else:
                analyzer.load_from_xml(content)
        
        elif args.bucket:
            try:
                import boto3
            except ImportError:
                print("ERROR: boto3 required for --bucket option", file=sys.stderr)
                print("Install: pip install boto3", file=sys.stderr)
                sys.exit(1)
            
            session_kwargs = {}
            if args.profile:
                session_kwargs['profile_name'] = args.profile
            
            session = boto3.Session(**session_kwargs)
            s3 = session.client('s3', endpoint_url=args.endpoint_url)
            
            response = s3.get_bucket_lifecycle_configuration(Bucket=args.bucket)
            analyzer.load_from_json(response)
    
    except FileNotFoundError:
        print(f"ERROR: File not found: {args.file}", file=sys.stderr)
        sys.exit(1)
    except json.JSONDecodeError as e:
        print(f"ERROR: Invalid JSON: {e}", file=sys.stderr)
        sys.exit(1)
    except ET.ParseError as e:
        print(f"ERROR: Invalid XML: {e}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)
    
    # Analyze
    analyzer.analyze()
    
    # Generate report
    report = analyzer.generate_report(format=args.output)
    print(report)
    
    # Exit code based on severity
    if any(i.severity == 'CRITICAL' for i in analyzer.issues):
        sys.exit(2)
    elif any(i.severity == 'WARNING' for i in analyzer.issues):
        sys.exit(1)
    else:
        sys.exit(0)


if __name__ == '__main__':
    main()

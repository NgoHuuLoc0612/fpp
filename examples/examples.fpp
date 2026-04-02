// ═══════════════════════════════════════════════════════════════
// F++ Example Programs — examples.fpp
// Demonstrates the full breadth of the F++ language
// ═══════════════════════════════════════════════════════════════

// ─── 1. Basic Types and Variables ────────────────────────────────
module basics {
    pub fn demo() {
        let a: i64   = 42
        let b: f64   = 3.14159
        let c: bool  = true
        let d: str   = "hello, F++!"
        let e: char  = 'A'

        // Type inference
        let x = 100       // inferred: i64
        let y = 2.718     // inferred: f64
        let z = "world"   // inferred: str

        // Mutable variables
        var counter = 0
        counter += 1
        counter += 1

        // Tuple destructuring
        let (p, q) = (10, 20)
        let sum = p + q

        println("basics: sum = ${sum}, counter = ${counter}")
    }
}

// ─── 2. Control Flow ─────────────────────────────────────────────
module control_flow {
    pub fn demo() {
        // If-else as expression
        let x = 42
        let description = if x > 100 { "big" }
                          else if x > 10 { "medium" }
                          else { "small" }

        // While loop with break/continue
        var i = 0
        var sum = 0
        while true {
            if i >= 10 { break }
            if i % 2 != 0 { i += 1; continue }
            sum += i
            i += 1
        }

        // For loop over array
        let fruits = ["apple", "banana", "cherry"]
        for fruit in fruits {
            println("fruit: ${fruit}")
        }

        // Labeled break with value
        let result = 'outer: {
            var n = 0
            while n < 100 {
                if n * n > 200 { break 'outer n }
                n += 1
            }
            0
        }
        println("control_flow: even_sum=${sum} label_break=${result}")
    }
}

// ─── 3. Functions ─────────────────────────────────────────────────
module functions {
    // Named function
    fn add(a: i64, b: i64) -> i64 { a + b }

    // Default parameter via overloading
    fn greet(name: str) -> str { "Hello, ${name}!" }
    fn greet_formal(name: str, title: str) -> str { "Good day, ${title} ${name}." }

    // Recursive function
    fn power(base: i64, exp: i64) -> i64 {
        if exp == 0 { 1 }
        else if exp % 2 == 0 {
            let half = power(base, exp / 2)
            half * half
        } else {
            base * power(base, exp - 1)
        }
    }

    // Higher-order functions
    fn apply_twice(f: fn(i64) -> i64, x: i64) -> i64 {
        f(f(x))
    }

    // Variadic function (simplified)
    fn sum_all(nums: [i64]) -> i64 {
        var total = 0
        for n in nums { total += n }
        total
    }

    pub fn demo() {
        let result = add(10, 32)
        let p = power(2, 10)
        let double = fn(x: i64) -> i64 { x * 2 }
        let quad = apply_twice(double, 5)
        let total = sum_all([1, 2, 3, 4, 5])
        println("functions: add=${result} power=${p} quad=${quad} sum=${total}")
    }
}

// ─── 4. Closures and Lambdas ──────────────────────────────────────
module closures {
    fn make_counter(start: i64) -> fn() -> i64 {
        var count = start
        fn() -> i64 {
            count += 1
            count
        }
    }

    fn make_multiplier(factor: i64) -> fn(i64) -> i64 {
        lambda(x) => x * factor
    }

    fn compose<A, B, C>(f: fn(A)->B, g: fn(B)->C) -> fn(A)->C {
        lambda(x) => g(f(x))
    }

    pub fn demo() {
        let counter = make_counter(0)
        let c1 = counter()   // 1
        let c2 = counter()   // 2
        let c3 = counter()   // 3

        let triple = make_multiplier(3)
        let add10 = lambda(x) => x + 10

        let triple_then_add10 = compose(triple, add10)
        let r = triple_then_add10(5)   // 3*5+10 = 25

        println("closures: c1=${c1} c2=${c2} c3=${c3} compose=${r}")
    }
}

// ─── 5. Structs ───────────────────────────────────────────────────
module structs {
    struct Vec2 {
        pub x: f64,
        pub y: f64,
    }

    struct Vec3 {
        pub x: f64,
        pub y: f64,
        pub z: f64,
    }

    struct Matrix2x2 {
        pub a: f64, pub b: f64,
        pub c: f64, pub d: f64,
    }

    impl Vec2 {
        pub fn new(x: f64, y: f64) -> Vec2 { Vec2 { x: x, y: y } }
        pub fn length(&self) -> f64 { sqrt(self.x*self.x + self.y*self.y) }
        pub fn dot(&self, other: Vec2) -> f64 { self.x*other.x + self.y*other.y }
        pub fn add(&self, other: Vec2) -> Vec2 { Vec2::new(self.x+other.x, self.y+other.y) }
        pub fn scale(&self, s: f64) -> Vec2 { Vec2::new(self.x*s, self.y*s) }
        pub fn normalize(&self) -> Vec2 {
            let len = self.length()
            if len < 1e-10 { Vec2::new(0.0, 0.0) } else { self.scale(1.0/len) }
        }
        pub fn to_str(&self) -> str { "(${self.x}, ${self.y})" }
    }

    impl Matrix2x2 {
        pub fn det(&self) -> f64 { self.a*self.d - self.b*self.c }
        pub fn trace(&self) -> f64 { self.a + self.d }
        pub fn mul(&self, v: Vec2) -> Vec2 {
            Vec2::new(self.a*v.x + self.b*v.y, self.c*v.x + self.d*v.y)
        }
    }

    pub fn demo() {
        let v1 = Vec2::new(3.0, 4.0)
        let v2 = Vec2::new(1.0, 0.0)
        let len = v1.length()      // 5.0
        let dot = v1.dot(v2)       // 3.0
        let sum = v1.add(v2)
        let norm = v1.normalize()

        let m = Matrix2x2 { a: 1.0, b: 2.0, c: 3.0, d: 4.0 }
        let det = m.det()          // -2.0
        let mv = m.mul(v2)

        println("structs: |v1|=${len} dot=${dot} det=${det}")
    }
}

// ─── 6. Enums and Pattern Matching ────────────────────────────────
module enums {
    enum Shape {
        Circle(f64),                        // radius
        Rectangle(f64, f64),                // width, height
        Triangle(f64, f64, f64),            // sides
        Point,
    }

    enum Tree<T> {
        Leaf(T),
        Node { val: T, left: Tree<T>, right: Tree<T> },
        Empty,
    }

    fn area(s: Shape) -> f64 {
        match s {
            Shape::Circle(r) => 3.14159 * r * r,
            Shape::Rectangle(w, h) => w * h,
            Shape::Triangle(a, b, c) => {
                let sp = (a + b + c) / 2.0
                sqrt(sp * (sp-a) * (sp-b) * (sp-c))
            },
            Shape::Point => 0.0,
        }
    }

    fn tree_sum(t: Tree<i64>) -> i64 {
        match t {
            Tree::Empty => 0,
            Tree::Leaf(v) => v,
            Tree::Node { val, left, right } => val + tree_sum(left) + tree_sum(right),
        }
    }

    pub fn demo() {
        let shapes = [
            Shape::Circle(5.0),
            Shape::Rectangle(4.0, 6.0),
            Shape::Triangle(3.0, 4.0, 5.0),
            Shape::Point,
        ]

        for s in shapes {
            let a = area(s)
            println("  shape area = ${a}")
        }

        let tree = Tree::Node {
            val: 1,
            left: Tree::Node { val: 2, left: Tree::Leaf(4), right: Tree::Leaf(5) },
            right: Tree::Node { val: 3, left: Tree::Empty, right: Tree::Leaf(6) },
        }
        let tsum = tree_sum(tree)
        println("enums: tree_sum=${tsum}")
    }
}

// ─── 7. Traits ────────────────────────────────────────────────────
module traits {
    trait Printable {
        fn to_string(&self) -> str
        fn print(&self) { println(self.to_string()) }
    }

    trait Comparable<T> {
        fn compare(&self, other: &T) -> i64
        fn lt(&self, other: &T) -> bool { self.compare(other) < 0 }
        fn gt(&self, other: &T) -> bool { self.compare(other) > 0 }
        fn eq(&self, other: &T) -> bool { self.compare(other) == 0 }
    }

    trait Iterator<T> {
        fn next(&mut self) -> Option<T>
        fn map<U>(self, f: fn(T)->U) -> MappedIter<T, U>
        fn filter(self, f: fn(&T)->bool) -> FilteredIter<T>
        fn collect(self) -> [T]
    }

    struct RangeIter {
        cur: i64,
        end: i64,
        step: i64,
    }

    impl Iterator<i64> for RangeIter {
        fn next(&mut self) -> Option<i64> {
            if self.cur >= self.end { None }
            else {
                let v = self.cur
                self.cur += self.step
                Some(v)
            }
        }

        fn collect(self) -> [i64] {
            var result = []
            var it = self
            while true {
                match it.next() {
                    None => break,
                    Some(v) => push(result, v),
                }
            }
            result
        }
    }

    pub fn demo() {
        let mut iter = RangeIter { cur: 0, end: 10, step: 2 }
        let nums = iter.collect()
        println("traits: evens = ${len(nums)} items")
    }
}

// ─── 8. Error Handling ────────────────────────────────────────────
module errors {
    enum AppError {
        ParseError(str),
        NotFound(str),
        PermissionDenied,
        IoError { code: i64, msg: str },
    }

    impl AppError {
        fn message(&self) -> str {
            match self {
                AppError::ParseError(s) => "Parse error: ${s}",
                AppError::NotFound(s) => "Not found: ${s}",
                AppError::PermissionDenied => "Permission denied",
                AppError::IoError { code, msg } => "IO error ${code}: ${msg}",
            }
        }
    }

    fn parse_positive(s: str) -> Result<i64, AppError> {
        let r = parse_int(s)
        match r {
            Err(_) => Err(AppError::ParseError("not a number: ${s}")),
            Ok(n) => if n < 0 {
                Err(AppError::ParseError("negative: ${n}"))
            } else {
                Ok(n)
            }
        }
    }

    fn divide(a: i64, b: i64) -> Result<i64, AppError> {
        if b == 0 { Err(AppError::ParseError("division by zero")) }
        else { Ok(a / b) }
    }

    fn pipeline(input: str, divisor: str) -> Result<i64, AppError> {
        let a = parse_positive(input)?     // propagate error with ?
        let b = parse_positive(divisor)?
        divide(a, b)?
        Ok(a + b)
    }

    pub fn demo() {
        match pipeline("100", "5") {
            Ok(v)  => println("errors: result = ${v}"),
            Err(e) => println("errors: ${e.message()}"),
        }
        match pipeline("abc", "5") {
            Ok(v)  => println("errors: result = ${v}"),
            Err(e) => println("errors: caught = ${e.message()}"),
        }

        // try/catch
        try {
            let x = parse_positive("-5")?
            println("should not reach: ${x}")
        } catch (e: AppError) {
            println("errors: try-catch caught error")
        }
    }
}

// ─── 9. Generics ─────────────────────────────────────────────────
module generics {
    struct Stack<T> {
        items: [T],
    }

    impl<T> Stack<T> {
        pub fn new() -> Stack<T> { Stack { items: [] } }
        pub fn push(&mut self, item: T) { push(self.items, item) }
        pub fn pop(&mut self) -> Option<T> { pop(self.items) }
        pub fn peek(&self) -> Option<&T> {
            if len(self.items) == 0 { None } else { Some(self.items[len(self.items)-1]) }
        }
        pub fn is_empty(&self) -> bool { len(self.items) == 0 }
        pub fn size(&self) -> i64 { len(self.items) }
    }

    fn map_opt<A, B>(opt: Option<A>, f: fn(A)->B) -> Option<B> {
        match opt {
            None => None,
            Some(v) => Some(f(v)),
        }
    }

    fn filter_map<A, B>(arr: [A], f: fn(A)->Option<B>) -> [B] {
        var result: [B] = []
        for item in arr {
            match f(item) {
                Some(v) => push(result, v),
                None => {},
            }
        }
        result
    }

    fn zip_with<A, B, C>(a: [A], b: [B], f: fn(A, B)->C) -> [C] {
        var result: [C] = []
        let n = min(len(a), len(b))
        var i = 0
        while i < n {
            push(result, f(a[i], b[i]))
            i += 1
        }
        result
    }

    pub fn demo() {
        var stack: Stack<i64> = Stack::new()
        stack.push(10)
        stack.push(20)
        stack.push(30)
        let top = stack.pop()
        let size = stack.size()

        let doubled = map_opt(Some(21), lambda(x) => x * 2)
        let nums = [1, 2, 3, 4, 5, 6]
        let parsed = filter_map(nums, lambda(x) => if x % 2 == 0 { Some(x * x) } else { None })
        let sums = zip_with([1,2,3], [4,5,6], lambda(a,b) => a+b)

        println("generics: stack_size=${size} doubled=${doubled} filter_map_len=${len(parsed)}")
    }
}

// ─── 10. Async / Concurrency ──────────────────────────────────────
module async_demo {
    async fn fetch_data(url: str) -> str {
        // Simulated async operation
        "data from ${url}"
    }

    async fn process(x: i64) -> i64 {
        x * x + 1
    }

    async fn parallel_sum(nums: [i64]) -> i64 {
        let tasks = map(nums, lambda(n) => spawn process(n))
        var total = 0
        for task in tasks {
            let r = await task
            total += r
        }
        total
    }

    fn producer(ch: chan<i64>, count: i64) {
        var i = 0
        while i < count {
            chan_send(ch, i * i)
            i += 1
        }
        chan_close(ch)
    }

    fn consumer(ch: chan<i64>) -> i64 {
        var sum = 0
        while true {
            match chan_recv(ch) {
                None => break,
                Some(v) => sum += v,
            }
        }
        sum
    }

    pub fn demo() {
        // Channel-based producer-consumer
        let ch = make_chan(16)
        spawn producer(ch, 5)
        let total = consumer(ch)
        println("async: channel_sum=${total}")

        // Async/await
        let data = await fetch_data("https://example.com")
        println("async: fetched=\"${data}\"")
    }
}

// ─── 11. Classes ─────────────────────────────────────────────────
module classes {
    class Animal {
        pub name: str
        pub sound: str

        pub fn new(name: str, sound: str) -> Animal {
            Animal { name: name, sound: sound }
        }

        pub fn speak(&self) -> str {
            "${self.name} says ${self.sound}"
        }

        pub fn describe(&self) -> str {
            "I am ${self.name}"
        }
    }

    class Dog: Animal {
        breed: str

        pub fn new(name: str, breed: str) -> Dog {
            Dog { name: name, sound: "Woof", breed: breed }
        }

        pub fn speak(&self) -> str {
            "${self.name} (${self.breed}) barks: ${self.sound}!"
        }

        pub fn fetch(&self, item: str) -> str {
            "${self.name} fetches the ${item}!"
        }
    }

    class Cat: Animal {
        indoor: bool

        pub fn new(name: str, indoor: bool) -> Cat {
            Cat { name: name, sound: "Meow", indoor: indoor }
        }

        pub fn speak(&self) -> str {
            let where_ = if self.indoor { "indoor" } else { "outdoor" }
            "${self.name} (${where_} cat) says ${self.sound}"
        }
    }

    pub fn demo() {
        let animals: [Animal] = [
            Dog::new("Rex", "German Shepherd"),
            Cat::new("Whiskers", true),
            Animal::new("Parrot", "Squawk"),
        ]
        for a in animals {
            println("classes: ${a.speak()}")
        }
    }
}

// ─── 12. Pattern Matching Advanced ────────────────────────────────
module pattern_matching {
    enum Expr {
        Num(f64),
        Var(str),
        Add(Expr, Expr),
        Mul(Expr, Expr),
        Neg(Expr),
        Pow(Expr, Expr),
    }

    fn eval(expr: Expr, env: map<str, f64>) -> Result<f64, str> {
        match expr {
            Expr::Num(n) => Ok(n),
            Expr::Var(name) => match map_get(env, name) {
                None => Err("Undefined variable: ${name}"),
                Some(v) => Ok(v),
            },
            Expr::Add(l, r) => Ok(eval(l, env)? + eval(r, env)?),
            Expr::Mul(l, r) => Ok(eval(l, env)? * eval(r, env)?),
            Expr::Neg(e)    => Ok(-eval(e, env)?),
            Expr::Pow(b, e) => Ok(pow(eval(b, env)?, eval(e, env)?)),
        }
    }

    pub fn demo() {
        // (x + 2) * (x - 1) where x = 3
        let expr = Expr::Mul(
            Expr::Add(Expr::Var("x"), Expr::Num(2.0)),
            Expr::Add(Expr::Var("x"), Expr::Neg(Expr::Num(1.0)))
        )
        let env = {"x": 3.0}
        match eval(expr, env) {
            Ok(v)  => println("pattern_matching: expr result = ${v}"),
            Err(e) => println("pattern_matching: error = ${e}"),
        }
    }
}

// ─── 13. Algorithms showcase ──────────────────────────────────────
module algorithms {
    fn binary_search<T>(arr: [T], target: T) -> Option<i64> {
        var lo: i64 = 0
        var hi: i64 = len(arr) - 1
        while lo <= hi {
            let mid = lo + (hi - lo) / 2
            if arr[mid] == target { return Some(mid) }
            else if arr[mid] < target { lo = mid + 1 }
            else { hi = mid - 1 }
        }
        None
    }

    fn merge_sorted<T>(a: [T], b: [T]) -> [T] {
        var result: [T] = []
        var i = 0; var j = 0
        while i < len(a) && j < len(b) {
            if a[i] <= b[j] { push(result, a[i]); i += 1 }
            else { push(result, b[j]); j += 1 }
        }
        while i < len(a) { push(result, a[i]); i += 1 }
        while j < len(b) { push(result, b[j]); j += 1 }
        result
    }

    fn knapsack(weights: [i64], values: [i64], capacity: i64) -> i64 {
        let n = len(weights)
        var dp = [[0; capacity+1]; n+1]
        var i = 1
        while i <= n {
            var w = 0
            while w <= capacity {
                dp[i][w] = dp[i-1][w]
                if weights[i-1] <= w {
                    let take = values[i-1] + dp[i-1][w - weights[i-1]]
                    if take > dp[i][w] { dp[i][w] = take }
                }
                w += 1
            }
            i += 1
        }
        dp[n][capacity]
    }

    fn lcs_length(a: str, b: str) -> i64 {
        let m = len(a); let n = len(b)
        var dp = [[0; n+1]; m+1]
        var i = 1
        while i <= m {
            var j = 1
            while j <= n {
                if a[i-1] == b[j-1] { dp[i][j] = dp[i-1][j-1] + 1 }
                else { dp[i][j] = max(dp[i-1][j], dp[i][j-1]) }
                j += 1
            }
            i += 1
        }
        dp[m][n]
    }

    pub fn demo() {
        let sorted = [1, 3, 5, 7, 9, 11, 13, 15, 17, 19]
        let pos = binary_search(sorted, 11)
        println("algorithms: binary_search(11) = ${pos}")

        let a = [1, 3, 5, 7]; let b = [2, 4, 6, 8]
        let merged = merge_sorted(a, b)
        println("algorithms: merged len = ${len(merged)}")

        let weights = [2, 3, 4, 5]; let values = [3, 4, 5, 6]
        let best = knapsack(weights, values, 8)
        println("algorithms: knapsack = ${best}")
    }
}

// ─── Main entry point ─────────────────────────────────────────────
fn main() {
    println("═══════════════════════════════════════")
    println("  F++ Language Feature Demonstration   ")
    println("═══════════════════════════════════════\n")

    basics::demo()
    control_flow::demo()
    functions::demo()
    closures::demo()
    structs::demo()
    enums::demo()
    traits::demo()
    errors::demo()
    generics::demo()
    async_demo::demo()
    classes::demo()
    pattern_matching::demo()
    algorithms::demo()

    println("\n═══════════════════════════════════════")
    println("  All demos completed successfully! ✓  ")
    println("═══════════════════════════════════════")
}

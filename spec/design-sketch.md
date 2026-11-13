创建一个简洁,高效的编程语言, 类似c和zig;
# 基础类型:
* 无符号:u8,u16,u32,u64,u128,usize,
* 有符号:i8,i16,i32,i64,i128,isize,
浮点数:f16,f32,f64
元组:{}, // 里面是任意类型数值,长度是编译器可知
    ** 元组读取方式: 名 + "." + 下标数,比如  tup.0;  长度写作: tup.len
数组:*type, 数组就是一个指针, 但是在编译期可知的长度,
    ** 数组读取方式: 名 + "." + 下标数,比如  arr.0;  长度写作: arr.len

切片:[]type, 是特殊的结构体, 有两个字段:ptr 和 len,
结构体:struct{},
枚举:enum : 就是整数
联合体:union
带标签的联合体:就是一个联合体, 这个联合体元素是一个元组{tag,Type}, tag是标签
函数类型: 函数就是一个const的指针
空值:null 实际是bool, 空值联合体记作?type
错误:error ,实际是一个整数, 错误联合体记作!type

变量只分:可变和不可变, 不可变的必须编译器可知;

每一个{}就是一个作用域, 包括enum, union, struct

支持编译器计算: const: 凡是const 的都是编译期已知的

泛型特性: 使用 type 表示泛型;

模块化: 一个文件就是一个模块;
  ** 引用模块, const name = @mod("root.zx");


  关键字: module, fn

  变量定义: 使用关键字 var, const 来定义,
  函数也是一个变量(特殊指针), 用fn来表示, 定义时 const func = fn() void;


类型转换: (type)var ; 这与c类似, 没有隐式转化


控制流 if else
if(val) v {}
while(val) v {} // v 是联合体中的值 , *v 是取其指针,引用

for(slice) s {}  // s 是切片的值

if 和 while 都可以捕获 空值联合体和错误联合体的值

break 关键字: 可以跳出当前作用域, 同时可以返回值, 比如 break i; 其实作用域就是一个值,默认返回{}
continue: 下一个循环


实例:
// 导入模块, 使用改模块的值
const mod = @mod("std");

mod.val = 10;   // 赋值

var var1 = 10;
const constVar = "hello";

// 函数
// 类型是: fn(i32,i32) void;  特殊指针
const add = fn (a:i32,b:i32) void { return a + b};

const Person = struct{ name:[]const u8, age:i32};   // 字段有与变量相同的生命周期

const V = struct{ var i:i32 = 10;};   // 作用与的值, 有静态的生命周期

使用 const 修饰的变量或函数, 必须是编译器可运行的;  const var = add(1,1);
